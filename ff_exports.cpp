#include "pch.h"
#include "ffmpeg_api.h"
#include "ff_orient.h"
#include "ff_session.h"
#include "ff_util.h"

namespace {

FfSession* LockSession(int handle)
{
    FfSession* session = FfSessionManager::Instance().Get(handle);
    if (!session) {
        ff::SetLastError("invalid session handle");
    }
    return session;
}

struct SessionLock {
    FfSession* session = nullptr;
    std::unique_lock<std::mutex> lock;

    explicit SessionLock(int handle)
        : session(LockSession(handle))
        , lock(session ? std::unique_lock<std::mutex>{session->mutex} : std::unique_lock<std::mutex>{})
    {
    }

    explicit operator bool() const
    {
        return session != nullptr;
    }
};

} // namespace

int FF_STDCALL FF_Init(int use_cpu, int width, int height, int fps, int bitrate_kb)
{
    auto session = std::make_unique<FfSession>();
    if (!FfSessionInit(*session, use_cpu, width, height, fps, bitrate_kb)) {
        return 0;
    }
    return FfSessionManager::Instance().Create(std::move(session));
}

void FF_STDCALL FF_Free(int handle)
{
    if (handle == 0) {
        return;
    }
    auto session = FfSessionManager::Instance().Remove(handle);
    if (session) {
        std::lock_guard<std::mutex> lock(session->mutex);
        FfSessionDestroy(*session);
    }
    ff::ClearLastError();
}

const char* FF_STDCALL FF_GetLastErrorGBK()
{
    return ff::GetLastErrorGBK();
}

const char* FF_STDCALL FF_GetEncoderTypeGBK(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return ff::GetLastErrorGBK();
    }
    return ff::CopyToThreadTextBuffer(guard.session->encoder_type_gbk);
}

int FF_STDCALL FF_GetWidth(int handle)
{
    SessionLock guard(handle);
    return guard ? guard.session->enc_width : 0;
}

int FF_STDCALL FF_GetHeight(int handle)
{
    SessionLock guard(handle);
    return guard ? guard.session->enc_height : 0;
}

int FF_STDCALL FF_GetDecodedFrameWidth(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return 0;
    }
    auto& s = *guard.session;
    if (s.dec_frame_dims_valid) {
        return s.dec_frame_w;
    }
    return s.src_width;
}

int FF_STDCALL FF_GetDecodedFrameHeight(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return 0;
    }
    auto& s = *guard.session;
    if (s.dec_frame_dims_valid) {
        return s.dec_frame_h;
    }
    return s.src_height;
}

int FF_STDCALL FF_GetRenderFrameWidth(int handle)
{
    SessionLock guard(handle);
    return guard ? guard.session->src_width : 0;
}

int FF_STDCALL FF_GetRenderFrameHeight(int handle)
{
    SessionLock guard(handle);
    return guard ? guard.session->src_height : 0;
}

int FF_STDCALL FF_IsLandscape(int handle)
{
    SessionLock guard(handle);
    return guard ? FfOrientIsLandscape(*guard.session) : 0;
}

int FF_STDCALL FF_GetRealtimeFps(int handle)
{
    SessionLock guard(handle);
    return guard ? guard.session->realtime_fps : 0;
}

int FF_STDCALL FF_GetEncodeBitrate(int handle)
{
    SessionLock guard(handle);
    return guard ? guard.session->realtime_bitrate_kb : 0;
}

int FF_STDCALL FF_GetLastEncodeBytes(int handle)
{
    SessionLock guard(handle);
    return guard ? guard.session->last_encode_bytes : 0;
}

int FF_STDCALL FF_GetRenderBGRASize(int handle, int* render_size, int* h264_size)
{
    SessionLock guard(handle);
    if (!guard || !render_size || !h264_size) {
        ff::SetLastError("invalid parameters");
        return -1;
    }
    *render_size = guard.session->render_bgra_size;
    *h264_size = guard.session->h264_buffer_size;
    ff::ClearLastError();
    return 0;
}

int FF_STDCALL FF_EncodeFrame_BGRA(
    int handle,
    const uint8_t* bgra_frame,
    int capture_bytes,
    uint8_t* h264_out,
    int* in_out_max_len)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }
    return FfEncodeBgraFrame(*guard.session, bgra_frame, capture_bytes, h264_out, in_out_max_len);
}

int FF_STDCALL FF_EncodeFrame_D3D11(
    int handle,
    int d3d_texture_ptr,
    uint8_t* h264_out,
    int* in_out_max_len)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }

    return FfEncodeFrameFromD3D11Texture(
        *guard.session,
        d3d_texture_ptr,
        h264_out,
        in_out_max_len);
}

int FF_STDCALL FF_ProcessFrame_D3D11(
    int handle,
    int d3d_texture_ptr,
    uint8_t* render_buf,
    int render_capacity)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }

    return FfProcessFrameD3D11(*guard.session, d3d_texture_ptr, render_buf, render_capacity);
}

int FF_STDCALL FF_DecodePacket_RenderBGRA(
    int handle,
    const uint8_t* h264_data,
    int h264_len,
    uint8_t* render_buf,
    int render_capacity)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }

    auto& session = *guard.session;
    if (h264_len <= 0) {
        h264_len = session.last_encode_bytes;
    }
    if (h264_len <= 0) {
        ff::SetLastError("H264 length is zero");
        return -1;
    }

    return FfDecodeH264ToBgra(session, h264_data, h264_len, render_buf, render_capacity);
}

int FF_STDCALL FF_RecordBeginEx(int handle, const char* file_path_gbk, int record_audio)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }
    const int ret = FfRecordBegin(*guard.session, file_path_gbk, record_audio);

    return ret;
}

int FF_STDCALL FF_RecordHasAudio(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return 0;
    }
    return FfRecordHasAudio(*guard.session) ? 1 : 0;
}

void FF_STDCALL FF_RecordFrame_BGRA(int handle, const uint8_t* bgra_frame, int capture_bytes)
{
    SessionLock guard(handle);
    if (!guard) {
        return;
    }
    FfRecordFrameBgra(*guard.session, bgra_frame, capture_bytes);
}

void FF_STDCALL FF_RecordFrame_D3D11(int handle, int d3d_texture_ptr)
{
    SessionLock guard(handle);
    if (!guard) {
        return;
    }

    auto& session = *guard.session;
    if (FfCopyD3D11TextureToBgra(session, d3d_texture_ptr, session.scratch_bgra) < 0) {
        return;
    }

    FfRecordFrameBgra(session, session.scratch_bgra.data(), session.render_bgra_size);
}

void FF_STDCALL FF_RecordEnd(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return;
    }
    FfRecordEnd(*guard.session);
}

int FF_STDCALL FF_PlayOpenEx(int handle, const char* file_path_gbk, int play_audio)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }
    const int ret = FfPlayOpen(*guard.session, file_path_gbk, play_audio);

    return ret;
}

int FF_STDCALL FF_PlayOpen(int handle, const char* file_path_gbk)
{
    return FF_PlayOpenEx(handle, file_path_gbk, 1);
}

int FF_STDCALL FF_PlayReadBGRA(int handle, uint8_t* bgra_out, int* in_out_max_len)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }
    return FfPlayReadBgra(*guard.session, bgra_out, in_out_max_len);
}

void FF_STDCALL FF_PlayPause(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return;
    }
    FfPlayPause(*guard.session);
    ff::ClearLastError();
}

void FF_STDCALL FF_PlayResume(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return;
    }
    FfPlayResume(*guard.session);
    ff::ClearLastError();
}

int FF_STDCALL FF_PlaySetVolume(int handle, int volume_percent)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }
    return FfPlaySetVolume(*guard.session, volume_percent);
}

int FF_STDCALL FF_PlaySeekMs(int handle, int ms)
{
    SessionLock guard(handle);
    if (!guard) {
        return -1;
    }
    return FfPlaySeekRelativeMs(*guard.session, ms);
}

void FF_STDCALL FF_PlayClose(int handle)
{
    SessionLock guard(handle);
    if (!guard) {
        return;
    }
    FfPlayClose(*guard.session);
}
