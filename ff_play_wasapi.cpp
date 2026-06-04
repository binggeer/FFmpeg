#include "pch.h"
#include "ff_play_wasapi.h"
#include "ff_util.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "ole32.lib")

struct WasapiPlayback {
    Microsoft::WRL::ComPtr<IAudioClient> client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render;
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume_ctrl;
    WAVEFORMATEX* mix_format = nullptr;
    bool com_inited = false;
    int sample_rate = 0;
    int channels = 0;
    int block_align = 0;
    bool is_float = false;
    int bits = 16;
    UINT32 buffer_frames = 0;
    int volume_percent = 100;
    bool use_pcm_gain = false;
};

int ClampVolumePercent(int volume)
{
    if (volume < 0) {
        return 0;
    }
    if (volume > 100) {
        return 100;
    }
    return volume;
}

namespace {

bool ResolveMixFormat(const WAVEFORMATEX* fmt, bool* is_float, int* bits, int* channels)
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

void WriteSample(
    BYTE* frame_base,
    int ch,
    int bytes_per_ch,
    bool is_float,
    int bits,
    int16_t sample_s16)
{
    BYTE* ptr = frame_base + static_cast<size_t>(ch) * static_cast<size_t>(bytes_per_ch);
    if (is_float && bits == 32) {
        const float v = static_cast<float>(sample_s16) / 32767.0f;
        *reinterpret_cast<float*>(ptr) = v;
    } else if (!is_float && bits == 16) {
        *reinterpret_cast<int16_t*>(ptr) = sample_s16;
    } else if (!is_float && bits == 32) {
        *reinterpret_cast<int32_t*>(ptr) = static_cast<int32_t>(sample_s16) << 16;
    }
}

} // namespace

bool FfPlayWasapiOpen(WasapiPlayback** out_state, int* out_sample_rate, int* out_channels)
{
    if (!out_state) {
        return false;
    }
    *out_state = nullptr;

    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (co_hr != S_OK && co_hr != S_FALSE) {
        ff::SetLastError("play audio CoInitializeEx failed");
        return false;
    }

    auto* state = new WasapiPlayback();
    state->com_inited = (co_hr == S_OK);

    auto fail = [&](const char* msg) -> bool {
        ff::SetLastError(msg);
        FfPlayWasapiClose(state);
        return false;
    };

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {

        return fail("play audio MMDeviceEnumerator failed");
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        return fail("play audio default render endpoint failed");
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &state->client);
    if (FAILED(hr)) {
        return fail("play audio Activate IAudioClient failed");
    }

    hr = state->client->GetMixFormat(&state->mix_format);
    if (FAILED(hr) || !state->mix_format) {
        return fail("play audio GetMixFormat failed");
    }

    if (!ResolveMixFormat(state->mix_format, &state->is_float, &state->bits, &state->channels)) {

        return fail("play audio unsupported mix format");
    }

    state->sample_rate = static_cast<int>(state->mix_format->nSamplesPerSec);
    state->block_align = state->mix_format->nBlockAlign;
    if (state->block_align <= 0) {
        state->block_align = state->channels * (state->bits / 8);
    }
    if (state->channels <= 0) {
        state->channels = 2;
    }

    REFERENCE_TIME buffer_duration = 10000000;
    hr = state->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        buffer_duration,
        0,
        state->mix_format,
        nullptr);
    if (FAILED(hr)) {
        return fail("play audio WASAPI Initialize failed");
    }

    hr = state->client->GetBufferSize(&state->buffer_frames);
    if (FAILED(hr) || state->buffer_frames == 0) {
        return fail("play audio GetBufferSize failed");
    }


    hr = state->client->GetService(IID_PPV_ARGS(&state->render));
    if (FAILED(hr)) {
        return fail("play audio GetService render failed");
    }

    hr = state->client->GetService(IID_PPV_ARGS(&state->volume_ctrl));
    if (FAILED(hr) || !state->volume_ctrl) {
        state->volume_ctrl.Reset();
        state->use_pcm_gain = true;

    } else {
        state->use_pcm_gain = false;
    }

    hr = state->client->Start();
    if (FAILED(hr)) {
        return fail("play audio WASAPI Start failed");
    }

    if (out_sample_rate) {
        *out_sample_rate = state->sample_rate;
    }
    if (out_channels) {
        *out_channels = state->channels;
    }

    *out_state = state;
    FfPlayWasapiSetVolume(state, state->volume_percent);

    ff::ClearLastError();
    return true;
}

bool FfPlayWasapiSetVolume(WasapiPlayback* state, int volume_percent)
{
    if (!state) {
        ff::SetLastError("play audio volume invalid state");
        return false;
    }

    state->volume_percent = ClampVolumePercent(volume_percent);
    const float level = static_cast<float>(state->volume_percent) / 100.0f;

    if (state->volume_ctrl) {
        const HRESULT hr = state->volume_ctrl->SetMasterVolume(level, nullptr);
        if (FAILED(hr)) {
            ff::SetLastError("play audio SetMasterVolume failed");

            return false;
        }

        ff::ClearLastError();
        return true;
    }

    ff::ClearLastError();
    return true;
}

void FfPlayWasapiClose(WasapiPlayback* state)
{
    if (!state) {
        return;
    }

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
}

void FfPlayWasapiReset(WasapiPlayback* state)
{
    if (!state || !state->client) {
        return;
    }

    state->client->Stop();
    state->client->Reset();
    state->client->Start();
}

void FfPlayWasapiPause(WasapiPlayback* state)
{
    if (!state || !state->client) {
        return;
    }
    state->client->Stop();
}

void FfPlayWasapiResume(WasapiPlayback* state)
{
    if (!state || !state->client) {
        return;
    }
    state->client->Start();
}

bool FfPlayWasapiWritePcm(WasapiPlayback* state, const uint8_t* pcm, int num_frames, int channels)
{
    if (!state || !state->client || !state->render || !pcm || num_frames <= 0 || channels <= 0) {
        ff::SetLastError("play audio write invalid args");
        return false;
    }

    const int bytes_per_ch = state->bits / 8;
    if (bytes_per_ch <= 0) {
        ff::SetLastError("play audio invalid bits per sample");
        return false;
    }

    int frame_stride = state->block_align;
    if (frame_stride < channels * bytes_per_ch) {
        frame_stride = channels * bytes_per_ch;
    }

    const int out_ch = std::min(channels, state->channels);
    const int16_t* src = reinterpret_cast<const int16_t*>(pcm);
    const float pcm_gain = state->use_pcm_gain ? (static_cast<float>(state->volume_percent) / 100.0f) : 1.0f;
    int frames_left = num_frames;
    int src_frame = 0;

    while (frames_left > 0) {
        UINT32 padding = 0;
        HRESULT hr = state->client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            ff::SetLastError("play audio GetCurrentPadding failed");

            return false;
        }

        UINT32 space = 0;
        if (padding < state->buffer_frames) {
            space = state->buffer_frames - padding;
        }
        if (space == 0) {
            Sleep(2);
            continue;
        }

        const UINT32 chunk = static_cast<UINT32>(std::min<int>(frames_left, static_cast<int>(space)));
        BYTE* dst = nullptr;
        hr = state->render->GetBuffer(chunk, &dst);
        if (FAILED(hr) || !dst) {
            ff::SetLastError("play audio GetBuffer failed");

            return false;
        }

        for (UINT32 frame = 0; frame < chunk; ++frame) {
            BYTE* frame_base = dst + static_cast<size_t>(frame) * static_cast<size_t>(frame_stride);
            for (int ch = 0; ch < state->channels; ++ch) {
                int16_t sample = 0;
                if (ch < out_ch) {
                    sample = src[(static_cast<size_t>(src_frame + frame) * static_cast<size_t>(channels))
                        + static_cast<size_t>(ch)];
                }
                if (pcm_gain < 0.999f || pcm_gain > 1.001f) {
                    const int scaled = static_cast<int>(static_cast<float>(sample) * pcm_gain);
                    if (scaled > 32767) {
                        sample = 32767;
                    } else if (scaled < -32768) {
                        sample = -32768;
                    } else {
                        sample = static_cast<int16_t>(scaled);
                    }
                }
                WriteSample(frame_base, ch, bytes_per_ch, state->is_float, state->bits, sample);
            }
        }

        hr = state->render->ReleaseBuffer(chunk, 0);
        if (FAILED(hr)) {
            ff::SetLastError("play audio ReleaseBuffer failed");
            return false;
        }

        src_frame += static_cast<int>(chunk);
        frames_left -= static_cast<int>(chunk);
    }

    ff::ClearLastError();
    return true;
}
