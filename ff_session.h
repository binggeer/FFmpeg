#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct FfSession {
    std::mutex mutex;

    int src_width = 0;
    int src_height = 0;
    int enc_width = 0;
    int enc_height = 0;
    int encoder_fps = 30;
    int rc_fps = 30;
    int target_fps = 0;
    int enc_scale_denom = 1;
    int bitrate_kb = 2500;
    bool use_cpu_only = false;

    std::string encoder_type_gbk;

    int last_encode_bytes = 0;
    int render_bgra_size = 0;
    int h264_buffer_size = 0;

    std::chrono::steady_clock::time_point stats_start{};
    std::chrono::steady_clock::time_point pace_next_deadline{};
    bool pace_deadline_active = false;
    int stats_frame_count = 0;
    int64_t stats_byte_count = 0;
    int realtime_fps = 0;
    int realtime_bitrate_kb = 0;
    int64_t enc_pts = 0;

    AVCodecContext* enc_ctx = nullptr;
    AVFrame* enc_frame_yuv = nullptr;
    AVFrame* enc_frame_yuv_alt = nullptr;
    int enc_yuv_write_idx = 0;
    AVFrame* enc_frame_bgra = nullptr;
    AVPacket* enc_pkt = nullptr;
    SwsContext* sws_enc = nullptr;

    AVCodecContext* dec_ctx = nullptr;
    AVFrame* dec_frame = nullptr;
    AVFrame* dec_frame_bgra = nullptr;
    AVPacket* dec_pkt = nullptr;
    SwsContext* sws_dec = nullptr;

    std::vector<uint8_t> pending_h264;
    bool dec_need_more = false;

    struct RecordState {
        AVFormatContext* fmt = nullptr;
        AVStream* stream = nullptr;
        AVCodecContext* enc = nullptr;
        AVFrame* frame_yuv = nullptr;
        AVFrame* frame_bgra = nullptr;
        AVPacket* pkt = nullptr;
        SwsContext* sws = nullptr;
        int64_t next_pts = 0;
        bool active = false;

        bool has_audio = false;
        AVStream* audio_stream = nullptr;
        AVCodecContext* audio_enc = nullptr;
        AVFrame* audio_frame = nullptr;
        AVPacket* audio_pkt = nullptr;
        SwrContext* swr = nullptr;
        int audio_sample_rate = 0;
        int audio_channels = 0;
        int audio_frame_samples = 0;
        int64_t audio_pts = 0;
        std::vector<uint8_t> audio_pcm_pending;
        void* wasapi_capture = nullptr;
    } record;

    struct PlayState {
        AVFormatContext* fmt = nullptr;
        AVCodecContext* dec = nullptr;
        AVCodecContext* audio_dec = nullptr;
        AVFrame* frame = nullptr;
        AVFrame* audio_frame = nullptr;
        AVFrame* frame_bgra = nullptr;
        AVPacket* pkt = nullptr;
        SwsContext* sws = nullptr;
        SwrContext* audio_swr = nullptr;
        int sws_src_w = 0;
        int sws_src_h = 0;
        int sws_src_fmt = -1;
        int video_stream = -1;
        int audio_stream = -1;
        int width = 0;
        int height = 0;
        bool active = false;
        bool has_audio = false;
        int audio_device_rate = 0;
        int audio_device_channels = 0;
        bool paused = false;
        int64_t pause_started_wall_ms = 0;
        int64_t last_pts = 0;
        int64_t pace_frame_duration_ms = 40;
        bool pace_started = false;
        int64_t pace_base_pts_ms = 0;
        int64_t pace_base_wall_ms = 0;
        int64_t pace_last_pts_ms = -1;
        uint64_t pace_no_pts_index = 0;
        void* wasapi_play = nullptr;
        int volume_percent = 100;
        std::vector<uint8_t> audio_pcm_scratch;
    } play;

    std::vector<uint8_t> scratch_bgra;
    std::vector<uint8_t> scratch_h264;

    bool preview_bgra_valid = false;
    bool use_decode_preview = false;
    uint64_t preview_serial = 0;
    uint64_t last_fed_preview_serial = 0;
    uint64_t last_packet_preview_serial = 0;
    int last_h264_stored = 0;

    bool orient_valid = false;
    int orient_landscape = 0;
    int dec_frame_w = 0;
    int dec_frame_h = 0;
    bool dec_frame_dims_valid = false;

    bool content_rect_valid = false;
    int content_x0 = 0;
    int content_y0 = 0;
    int content_x1 = 0;
    int content_y1 = 0;
    int dec_sws_src_w = 0;
    int dec_sws_src_h = 0;
    int dec_sws_dst_w = 0;
    int dec_sws_dst_h = 0;

    void* d3d_device = nullptr;
    void* d3d_context = nullptr;
    void* d3d_staging = nullptr;
    int d3d_staging_w = 0;
    int d3d_staging_h = 0;
    unsigned d3d_staging_format = 0;
    bool d3d_mapped = false;
};

class FfSessionManager {
public:
    static FfSessionManager& Instance();

    int Create(std::unique_ptr<FfSession> session);
    FfSession* Get(int handle);
    std::unique_ptr<FfSession> Remove(int handle);

private:
    std::mutex map_mutex_;
    std::unordered_map<int, std::unique_ptr<FfSession>> sessions_;
    std::atomic<int> next_handle_{1};
};

bool FfSessionInit(FfSession& session, int use_cpu, int width, int height, int fps, int bitrate_kb);
void FfSessionDestroy(FfSession& session);
bool FfEnsureDecoder(FfSession& session);

void FfSessionOnFrameDone(FfSession& session, int encoded_bytes);
void FfSessionThrottle(FfSession& session, bool enable);

int FfCopyD3D11TextureToBgra(FfSession& session, int texture_ptr, std::vector<uint8_t>& out);
bool FfBeginD3D11Read(FfSession& session, int texture_ptr, const uint8_t** out_data, int* out_pitch);
void FfEndD3D11Read(FfSession& session);
void FfCopyMappedBgraToScratch(FfSession& session, const uint8_t* mapped, int pitch);

int FfEncodeBgraFrame(FfSession& session, const uint8_t* bgra, int capture_bytes, uint8_t* h264_out, int* in_out_len);
int FfEncodeBgraFramePitched(
    FfSession& session,
    const uint8_t* bgra,
    int src_pitch,
    int capture_bytes,
    uint8_t* h264_out,
    int* in_out_len);
int FfEncodeFrameFromD3D11Texture(
    FfSession& session,
    int texture_ptr,
    uint8_t* h264_out,
    int* in_out_len);
int FfProcessFrameD3D11(FfSession& session, int texture_ptr, uint8_t* render_buf, int render_capacity);
int FfDecodeH264ToBgra(FfSession& session, const uint8_t* h264, int h264_len, uint8_t* bgra_out, int bgra_capacity);

int FfRecordBegin(FfSession& session, const char* path_gbk, int record_audio);
bool FfRecordHasAudio(const FfSession& session);
void FfRecordFrameBgra(FfSession& session, const uint8_t* bgra, int capture_bytes);
void FfRecordEnd(FfSession& session);
bool FfRecordFeedAudioPcm16(FfSession& session, const uint8_t* pcm, int byte_len);

int FfPlayOpen(FfSession& session, const char* path_gbk, int play_audio);
int FfPlayReadBgra(FfSession& session, uint8_t* bgra_out, int* in_out_len);
void FfPlayClose(FfSession& session);
void FfPlayResetPace(FfSession& session);
void FfPlayPause(FfSession& session);
void FfPlayResume(FfSession& session);
int FfPlaySetVolume(FfSession& session, int volume_percent);
int FfPlaySeekRelativeMs(FfSession& session, int ms_delta);

void FfReleaseCodec(AVCodecContext** ctx);
void FfReleaseFrame(AVFrame** frame);
void FfReleasePacket(AVPacket** pkt);
void FfReleaseSws(SwsContext** sws);
