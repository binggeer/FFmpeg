#pragma once

#include <cstdint>

#ifdef FFMPEG_EXPORTS
#define FF_API extern "C"
#else
#define FF_API extern "C" __declspec(dllimport)
#endif

#define FF_STDCALL __stdcall

FF_API int FF_STDCALL FF_Init(int use_cpu, int width, int height, int fps, int bitrate_kb);
FF_API void FF_STDCALL FF_Free(int handle);

FF_API const char* FF_STDCALL FF_GetLastErrorGBK();
FF_API const char* FF_STDCALL FF_GetEncoderTypeGBK(int handle);

FF_API int FF_STDCALL FF_GetWidth(int handle);
FF_API int FF_STDCALL FF_GetHeight(int handle);
FF_API int FF_STDCALL FF_GetDecodedFrameWidth(int handle);
FF_API int FF_STDCALL FF_GetDecodedFrameHeight(int handle);
FF_API int FF_STDCALL FF_GetRenderFrameWidth(int handle);
FF_API int FF_STDCALL FF_GetRenderFrameHeight(int handle);
FF_API int FF_STDCALL FF_IsLandscape(int handle);
FF_API int FF_STDCALL FF_GetRealtimeFps(int handle);
FF_API int FF_STDCALL FF_GetEncodeBitrate(int handle);
FF_API int FF_STDCALL FF_GetLastEncodeBytes(int handle);

FF_API int FF_STDCALL FF_GetRenderBGRASize(int handle, int* render_size, int* h264_size);

FF_API int FF_STDCALL FF_EncodeFrame_BGRA(
    int handle,
    const uint8_t* bgra_frame,
    int capture_bytes,
    uint8_t* h264_out,
    int* in_out_max_len);

FF_API int FF_STDCALL FF_EncodeFrame_D3D11(
    int handle,
    int d3d_texture_ptr,
    uint8_t* h264_out,
    int* in_out_max_len);

FF_API int FF_STDCALL FF_ProcessFrame_D3D11(
    int handle,
    int d3d_texture_ptr,
    uint8_t* render_buf,
    int render_capacity);

FF_API int FF_STDCALL FF_DecodePacket_RenderBGRA(
    int handle,
    const uint8_t* h264_data,
    int h264_len,
    uint8_t* render_buf,
    int render_capacity);

/* record_audio: 0=video only, non-zero=loopback AAC */
FF_API int FF_STDCALL FF_RecordBeginEx(int handle, const char* file_path_gbk, int record_audio);
FF_API int FF_STDCALL FF_RecordHasAudio(int handle);
FF_API void FF_STDCALL FF_RecordFrame_BGRA(int handle, const uint8_t* bgra_frame, int capture_bytes);
FF_API void FF_STDCALL FF_RecordFrame_D3D11(int handle, int d3d_texture_ptr);
FF_API void FF_STDCALL FF_RecordEnd(int handle);

/* play_audio: 0=video only, non-zero=WASAPI speaker output */
FF_API int FF_STDCALL FF_PlayOpenEx(int handle, const char* file_path_gbk, int play_audio);
FF_API int FF_STDCALL FF_PlayOpen(int handle, const char* file_path_gbk);
/* PlayReadBGRA: 0=frame ok, 1=EOF, 2=paused, <0=error */
FF_API int FF_STDCALL FF_PlayReadBGRA(int handle, uint8_t* bgra_out, int* in_out_max_len);
FF_API void FF_STDCALL FF_PlayPause(int handle);
FF_API void FF_STDCALL FF_PlayResume(int handle);
FF_API int FF_STDCALL FF_PlaySetVolume(int handle, int volume_percent);
FF_API int FF_STDCALL FF_PlaySeekMs(int handle, int ms);
FF_API void FF_STDCALL FF_PlayClose(int handle);
