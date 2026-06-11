#include "pch.h"
#include "ff_record_wasapi.h"
#include "ff_session.h"
#include "ff_util.h"

#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace {

using Microsoft::WRL::ComPtr;
struct WasapiLoopback {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioEndpointVolume> endpoint_volume;
    WAVEFORMATEX* mix_format = nullptr;
    bool com_inited = false;
    int capture_channels = 0;
    int encode_channels = 0;
    int capture_block_align = 0;
    int bytes_per_video_frame = 0;
    bool capture_is_float = false;
    int capture_bits = 16;
    float volume_compensation_gain = 1.0f;
    std::vector<uint8_t> capture_queue;
    FfCriticalSection queue_cs;
    std::thread capture_thread;
    std::atomic<bool> capture_stop{false};
};

WasapiLoopback* GetWasapi(FfSession& session)
{
    return static_cast<WasapiLoopback*>(session.record.wasapi_capture);
}

void SetWasapi(FfSession& session, WasapiLoopback* state)
{
    session.record.wasapi_capture = state;
}

bool DrainWasapiPackets(WasapiLoopback* state, bool set_error_on_fail);

void StopWasapiCaptureThread(WasapiLoopback* state)
{
    if (!state) {
        return;
    }

    state->capture_stop.store(true, std::memory_order_release);
    if (state->capture_thread.joinable()) {
        state->capture_thread.join();
    }
}

void WasapiCaptureThreadProc(WasapiLoopback* state)
{
    while (!state->capture_stop.load(std::memory_order_acquire)) {
        {
            FfCsLock lock(state->queue_cs);
            DrainWasapiPackets(state, false);
        }
        Sleep(5);
    }
}

void StartWasapiCaptureThread(WasapiLoopback* state)
{
    if (!state) {
        return;
    }

    state->capture_stop.store(false, std::memory_order_release);
    state->capture_thread = std::thread(WasapiCaptureThreadProc, state);
}

void FreeWasapi(FfSession& session)
{
    WasapiLoopback* state = GetWasapi(session);
    if (!state) {
        return;
    }

    StopWasapiCaptureThread(state);

    if (state->client) {
        state->client->Stop();
    }
    if (state->mix_format) {
        CoTaskMemFree(state->mix_format);
        state->mix_format = nullptr;
    }
    if (state->com_inited) {
        CoUninitialize();
        state->com_inited = false;
    }

    delete state;
    SetWasapi(session, nullptr);
}

bool ResolveWasapiPcmFormat(const WAVEFORMATEX* fmt, bool* is_float, int* bits, int* channels)
{
    if (!fmt || !is_float || !bits || !channels) {
        return false;
    }

    *channels = fmt->nChannels;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        *is_float = false;
        *bits = fmt->wBitsPerSample;
        return *bits == 16 || *bits == 32;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        *is_float = true;
        *bits = fmt->wBitsPerSample;
        return *bits == 32;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            *is_float = true;
            *bits = fmt->wBitsPerSample;
            return *bits == 32;
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            *is_float = false;
            *bits = fmt->wBitsPerSample;
            return *bits == 16 || *bits == 32;
        }
    }
    return false;
}

constexpr float kRecordVolumeGainMax = 8.0f;
constexpr float kRecordVolumeScalarMin = 0.001f;

void UpdateRecordVolumeCompensation(WasapiLoopback* state)
{
    if (!state || !state->endpoint_volume) {
        return;
    }

    BOOL muted = FALSE;
    if (SUCCEEDED(state->endpoint_volume->GetMute(&muted)) && muted) {
        state->volume_compensation_gain = 1.0f;
        return;
    }

    float scalar = 1.0f;
    if (FAILED(state->endpoint_volume->GetMasterVolumeLevelScalar(&scalar))) {
        return;
    }

    if (scalar < kRecordVolumeScalarMin) {
        scalar = kRecordVolumeScalarMin;
    }

    float gain = 1.0f / scalar;
    if (gain > kRecordVolumeGainMax) {
        gain = kRecordVolumeGainMax;
    }
    state->volume_compensation_gain = gain;
}

int16_t ApplyRecordGainS16(int16_t sample, float gain)
{
    if (gain >= 0.999f && gain <= 1.001f) {
        return sample;
    }
    const float scaled = static_cast<float>(sample) * gain;
    if (scaled > 32767.0f) {
        return 32767;
    }
    if (scaled < -32768.0f) {
        return -32768;
    }
    return static_cast<int16_t>(scaled);
}

void AppendSilenceS16(std::vector<uint8_t>& out, int num_frames, int channels)
{
    if (num_frames <= 0 || channels <= 0) {
        return;
    }
    const size_t add = static_cast<size_t>(num_frames) * static_cast<size_t>(channels) * sizeof(int16_t);
    const size_t old_size = out.size();
    out.resize(old_size + add, 0);
}

void AppendPcmS16(
    std::vector<uint8_t>& out,
    const BYTE* data,
    UINT32 num_frames,
    int src_channels,
    int dst_channels,
    int block_align,
    bool is_float,
    int bits,
    float gain)
{
    if (!data || num_frames == 0 || src_channels <= 0 || dst_channels <= 0) {
        return;
    }

    const int bytes_per_ch = bits / 8;
    if (bytes_per_ch <= 0) {
        return;
    }

    int frame_stride = block_align;
    if (frame_stride < src_channels * bytes_per_ch) {
        frame_stride = src_channels * bytes_per_ch;
    }

    const int out_ch = std::min(src_channels, dst_channels);
    const int old_samples = static_cast<int>(out.size() / sizeof(int16_t));
    out.resize(out.size() + static_cast<size_t>(num_frames) * static_cast<size_t>(out_ch) * sizeof(int16_t));
    int16_t* dst = reinterpret_cast<int16_t*>(out.data()) + old_samples;

    auto write_sample = [&](int frame, int ch, int16_t sample) {
        if (ch >= out_ch) {
            return;
        }
        dst[static_cast<size_t>(frame) * static_cast<size_t>(out_ch) + static_cast<size_t>(ch)] = sample;
    };

    for (UINT32 frame = 0; frame < num_frames; ++frame) {
        const BYTE* frame_base = data + static_cast<size_t>(frame) * static_cast<size_t>(frame_stride);
        for (int ch = 0; ch < src_channels; ++ch) {
            const BYTE* sample_ptr = frame_base + static_cast<size_t>(ch) * static_cast<size_t>(bytes_per_ch);
            int16_t sample = 0;
            if (is_float && bits == 32) {
                float v = *reinterpret_cast<const float*>(sample_ptr);
                if (gain < 0.999f || gain > 1.001f) {
                    v *= gain;
                }
                if (v > 1.0f) {
                    v = 1.0f;
                } else if (v < -1.0f) {
                    v = -1.0f;
                }
                sample = static_cast<int16_t>(v * 32767.0f);
            } else if (!is_float && bits == 16) {
                sample = *reinterpret_cast<const int16_t*>(sample_ptr);
                sample = ApplyRecordGainS16(sample, gain);
            } else if (!is_float && bits == 32) {
                sample = static_cast<int16_t>(*reinterpret_cast<const int32_t*>(sample_ptr) >> 16);
                sample = ApplyRecordGainS16(sample, gain);
            } else {
                continue;
            }
            write_sample(static_cast<int>(frame), ch, sample);
        }
    }
}

bool DrainWasapiPackets(WasapiLoopback* state, bool set_error_on_fail)
{
    UpdateRecordVolumeCompensation(state);

    for (int guard = 0; guard < 256; ++guard) {
        UINT32 packet_length = 0;
        const HRESULT hr_size = state->capture->GetNextPacketSize(&packet_length);
        if (FAILED(hr_size)) {
            if (set_error_on_fail) {
                ff::SetLastError("record audio GetNextPacketSize failed");
            }
            return false;
        }
        if (packet_length == 0) {
            break;
        }

        BYTE* data = nullptr;
        UINT32 num_frames = 0;
        DWORD flags = 0;
        const HRESULT hr_buf = state->capture->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
        if (FAILED(hr_buf)) {
            if (set_error_on_fail) {
                ff::SetLastError("record audio GetBuffer failed");
            }
            return false;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            AppendSilenceS16(state->capture_queue, static_cast<int>(num_frames), state->encode_channels);
        } else if (data && num_frames > 0) {
            AppendPcmS16(
                state->capture_queue,
                data,
                num_frames,
                state->capture_channels,
                state->encode_channels,
                state->capture_block_align,
                state->capture_is_float,
                state->capture_bits,
                state->volume_compensation_gain);
        }

        state->capture->ReleaseBuffer(num_frames);
    }
    return true;
}

} // namespace

bool FfRecordWasapiOpen(FfSession& session, int& out_sample_rate, int& out_channels)
{
    FfRecordWasapiClose(session);

    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (co_hr != S_OK && co_hr != S_FALSE) {
        ff::SetLastError("record audio CoInitializeEx failed");
        return false;
    }

    auto* state = new WasapiLoopback();
    state->com_inited = (co_hr == S_OK);

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    auto fail = [&](const char* msg) -> bool {
        ff::SetLastError(msg);
        if (state->mix_format) {
            CoTaskMemFree(state->mix_format);
        }
        const bool com_inited = state->com_inited;
        delete state;
        if (com_inited) {
            CoUninitialize();
        }
        return false;
    };

    if (FAILED(hr)) {
        return fail("record audio MMDeviceEnumerator failed");
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        return fail("record audio default render endpoint failed");
    }

    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &state->endpoint_volume);
    if (FAILED(hr) || !state->endpoint_volume) {
        state->endpoint_volume.Reset();
    } else {
        UpdateRecordVolumeCompensation(state);
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &state->client);
    if (FAILED(hr)) {
        return fail("record audio Activate IAudioClient failed");
    }

    hr = state->client->GetMixFormat(&state->mix_format);
    if (FAILED(hr) || !state->mix_format) {
        return fail("record audio GetMixFormat failed");
    }

    if (!ResolveWasapiPcmFormat(
            state->mix_format,
            &state->capture_is_float,
            &state->capture_bits,
            &state->capture_channels)) {
        return fail("record audio unsupported mix format");
    }

    REFERENCE_TIME buffer_duration = 10000000;
    hr = state->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        buffer_duration,
        0,
        state->mix_format,
        nullptr);
    if (FAILED(hr)) {
        return fail("record audio WASAPI Initialize failed");
    }

    hr = state->client->GetService(IID_PPV_ARGS(&state->capture));
    if (FAILED(hr)) {
        return fail("record audio GetService capture failed");
    }

    hr = state->client->Start();
    if (FAILED(hr)) {
        return fail("record audio WASAPI Start failed");
    }

    out_sample_rate = static_cast<int>(state->mix_format->nSamplesPerSec);
    out_channels = state->capture_channels;
    if (out_channels <= 0) {
        out_channels = 2;
    }
    if (out_channels > 2) {
        out_channels = 2;
    }

    state->encode_channels = out_channels;
    state->capture_block_align = state->mix_format->nBlockAlign;
    if (state->capture_block_align <= 0) {
        state->capture_block_align = state->capture_channels * (state->capture_bits / 8);
    }

    const int fps = session.encoder_fps > 0 ? session.encoder_fps : 30;
    const int samples_per_frame = std::max(1, out_sample_rate / fps);
    state->bytes_per_video_frame = samples_per_frame * state->encode_channels * static_cast<int>(sizeof(int16_t));

    SetWasapi(session, state);
    DrainWasapiPackets(state, true);
    StartWasapiCaptureThread(state);

    ff::ClearLastError();
    return true;
}

void FfRecordWasapiClose(FfSession& session)
{
    FreeWasapi(session);
}

bool FfRecordWasapiGetEncodeFormat(const FfSession& session, int& out_sample_rate, int& out_channels)
{
    const WasapiLoopback* state = static_cast<const WasapiLoopback*>(session.record.wasapi_capture);
    if (!state || !state->mix_format) {
        return false;
    }

    out_sample_rate = static_cast<int>(state->mix_format->nSamplesPerSec);
    out_channels = state->encode_channels;
    if (out_channels <= 0) {
        out_channels = 2;
    }
    if (out_channels > 2) {
        out_channels = 2;
    }
    return out_sample_rate > 0 && out_channels > 0;
}

void FfRecordWasapiFlushRemaining(FfSession& session)
{
    WasapiLoopback* state = GetWasapi(session);
    if (!state || !session.record.has_audio) {
        return;
    }

    {
        FfCsLock lock(state->queue_cs);
        DrainWasapiPackets(state, true);

        if (!state->capture_queue.empty()) {
            const int bytes_per_frame =
                session.record.audio_channels * static_cast<int>(sizeof(int16_t));
            if (bytes_per_frame > 0) {
                const int aligned = static_cast<int>(
                    state->capture_queue.size() / static_cast<size_t>(bytes_per_frame))
                    * bytes_per_frame;
                if (aligned > 0) {
                    FfRecordFeedAudioPcm16(session, state->capture_queue.data(), aligned);
                }
            }
            state->capture_queue.clear();
        }
    }
}

bool FfRecordWasapiCaptureForVideoFrame(FfSession& session)
{
    WasapiLoopback* state = GetWasapi(session);
    if (!state || !session.record.has_audio || state->bytes_per_video_frame <= 0) {
        return true;
    }

    const int target_bytes = state->bytes_per_video_frame;

    {
        FfCsLock lock(state->queue_cs);
        if (!DrainWasapiPackets(state, true)) {
            return false;
        }

        int wait_ms = 0;
        while (static_cast<int>(state->capture_queue.size()) < target_bytes && wait_ms < 80) {
            lock.release();
            Sleep(1);
            ++wait_ms;
            lock.acquire(state->queue_cs);
            DrainWasapiPackets(state, false);
        }

        std::vector<uint8_t> frame_pcm;
        frame_pcm.reserve(static_cast<size_t>(target_bytes));

        if (static_cast<int>(state->capture_queue.size()) >= target_bytes) {
            frame_pcm.assign(state->capture_queue.begin(), state->capture_queue.begin() + target_bytes);
            state->capture_queue.erase(
                state->capture_queue.begin(),
                state->capture_queue.begin() + target_bytes);
        } else {
            frame_pcm = state->capture_queue;
            state->capture_queue.clear();
            frame_pcm.resize(static_cast<size_t>(target_bytes), 0);
        }

        if (!FfRecordFeedAudioPcm16(session, frame_pcm.data(), target_bytes)) {
            return false;
        }
    }

    ff::ClearLastError();
    return true;
}
