#include "pch.h"
#include "ff_session.h"
#include "ff_orient.h"
#include "ff_util.h"

#include <d3d11.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "d3d11.lib")

bool FfSafeAvcodecOpen2(AVCodecContext* ctx, const AVCodec* codec, int* out_ret)
{
    if (!ctx || !codec || !out_ret) {
        return false;
    }

    *out_ret = AVERROR_UNKNOWN;
    __try {
        *out_ret = avcodec_open2(ctx, codec, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out_ret = AVERROR_EXTERNAL;
        return false;
    }
    return true;
}

void FfSafeAvcodecFreeContext(AVCodecContext** ctx)
{
    if (!ctx || !*ctx) {
        return;
    }

    __try {
        avcodec_free_context(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ctx = nullptr;
    }
}

namespace {

void SetAvError(const char* prefix, int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    ff::SetLastError(std::string(prefix) + buf);
}

constexpr int64_t kMaxInitDimension = 8192;
constexpr int64_t kMaxInitBufferBytes = 256LL * 1024 * 1024;

bool ValidateInitBufferSizes(int width, int height, int enc_width, int enc_height)
{
    if (width <= 0 || height <= 0 || enc_width <= 0 || enc_height <= 0) {
        return false;
    }
    if (width > kMaxInitDimension || height > kMaxInitDimension) {
        return false;
    }

    const int64_t render_size = static_cast<int64_t>(width) * 4 * height;
    const int64_t h264_size = static_cast<int64_t>(enc_width) * enc_height * 2;
    return render_size > 0
        && h264_size > 0
        && render_size <= kMaxInitBufferBytes
        && h264_size <= kMaxInitBufferBytes;
}

std::string BuildEncoderDescription(const AVCodec* codec)
{
    if (!codec || strcmp(codec->name, "libx264") == 0) {
        return ff::Utf8ToGbk("CPU");
    }

    // Avoid DXGI adapter enumeration during init; it can AV-crash on some drivers.
    return ff::Utf8ToGbk(std::string("GPU(") + codec->name + ")");
}

bool InitScaler(
    SwsContext** sws,
    int src_w,
    int src_h,
    AVPixelFormat src_fmt,
    int dst_w,
    int dst_h,
    AVPixelFormat dst_fmt,
    int flags = SWS_FAST_BILINEAR)
{
    *sws = sws_getContext(
        src_w, src_h, src_fmt,
        dst_w, dst_h, dst_fmt,
        flags, nullptr, nullptr, nullptr);
    return *sws != nullptr;
}

void ComputeAspectFit(
    int src_w,
    int src_h,
    int dst_w,
    int dst_h,
    int* fit_w,
    int* fit_h,
    int* off_x,
    int* off_y)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        *fit_w = dst_w;
        *fit_h = dst_h;
        *off_x = 0;
        *off_y = 0;
        return;
    }

    const int64_t num_w = static_cast<int64_t>(dst_w) * src_h;
    const int64_t num_h = static_cast<int64_t>(dst_h) * src_w;
    if (num_w >= num_h) {
        *fit_w = dst_w;
        *fit_h = static_cast<int>((static_cast<int64_t>(dst_w) * src_h + src_w - 1) / src_w);
        *off_x = 0;
        *off_y = (dst_h - *fit_h) / 2;
    } else {
        *fit_h = dst_h;
        *fit_w = static_cast<int>((static_cast<int64_t>(dst_h) * src_w + src_h - 1) / src_h);
        *off_x = (dst_w - *fit_w) / 2;
        *off_y = 0;
    }

    if (*fit_w < 1) {
        *fit_w = 1;
    }
    if (*fit_h < 1) {
        *fit_h = 1;
    }
    if (*off_x < 0) {
        *off_x = 0;
    }
    if (*off_y < 0) {
        *off_y = 0;
    }
    if (*off_x + *fit_w > dst_w) {
        *fit_w = dst_w - *off_x;
    }
    if (*off_y + *fit_h > dst_h) {
        *fit_h = dst_h - *off_y;
    }
}

void PrepareCroppedAvSrc(
    AVFrame* frame,
    int x0,
    int y0,
    const uint8_t* src_data[4],
    int src_linesize[4])
{
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);
    int h_shift = 0;
    int v_shift = 0;
    av_pix_fmt_get_chroma_sub_sample(fmt, &h_shift, &v_shift);

    for (int plane = 0; plane < 4; ++plane) {
        src_data[plane] = nullptr;
        src_linesize[plane] = 0;
        if (!frame->data[plane]) {
            continue;
        }

        const int plane_x = x0 >> (plane ? h_shift : 0);
        const int plane_y = y0 >> (plane ? v_shift : 0);
        src_data[plane] = frame->data[plane]
            + static_cast<size_t>(plane_y) * static_cast<size_t>(frame->linesize[plane])
            + static_cast<size_t>(plane_x);
        src_linesize[plane] = frame->linesize[plane];
    }
}

void EnsureDecSws(
    FfSession& session,
    AVFrame* src,
    int crop_w,
    int crop_h,
    int fit_w,
    int fit_h)
{
    const auto src_fmt = static_cast<AVPixelFormat>(src->format);
    if (session.sws_dec
        && session.dec_sws_src_w == crop_w
        && session.dec_sws_src_h == crop_h
        && session.dec_sws_dst_w == fit_w
        && session.dec_sws_dst_h == fit_h) {
        return;
    }

    FfReleaseSws(&session.sws_dec);
    if (!InitScaler(
            &session.sws_dec,
            crop_w,
            crop_h,
            src_fmt,
            fit_w,
            fit_h,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR)) {
        session.dec_sws_src_w = 0;
        session.dec_sws_src_h = 0;
        session.dec_sws_dst_w = 0;
        session.dec_sws_dst_h = 0;
        return;
    }

    session.dec_sws_src_w = crop_w;
    session.dec_sws_src_h = crop_h;
    session.dec_sws_dst_w = fit_w;
    session.dec_sws_dst_h = fit_h;
}

void ApplyTargetBitrate(AVCodecContext* ctx, const AVCodec* codec, int64_t bitrate_bps, int bitrate_kb, int fps)
{
    ctx->bit_rate = bitrate_bps;
    ctx->rc_max_rate = bitrate_bps;
    ctx->rc_min_rate = bitrate_bps;

    const int eff_fps = fps > 0 ? fps : 30;
    const int64_t frame_bits = std::max<int64_t>(bitrate_bps / eff_fps, 50000);
    ctx->rc_buffer_size = static_cast<int>(std::min<int64_t>(bitrate_bps, frame_bits * 4));

    char rate_buf[32] = {};
    snprintf(rate_buf, sizeof(rate_buf), "%lld", static_cast<long long>(bitrate_bps));

    if (strcmp(codec->name, "h264_amf") == 0) {
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
        av_opt_set_int(ctx->priv_data, "b", bitrate_bps, 0);
        av_opt_set_int(ctx->priv_data, "maxrate", bitrate_bps, 0);
        av_opt_set_int(ctx->priv_data, "minrate", bitrate_bps, 0);
        av_opt_set_int(ctx->priv_data, "bufsize", bitrate_bps, 0);
    } else if (strcmp(codec->name, "h264_nvenc") == 0) {
        av_opt_set(ctx->priv_data, "bitrate", rate_buf, 0);
        av_opt_set(ctx->priv_data, "maxrate", rate_buf, 0);
        av_opt_set(ctx->priv_data, "bufsize", rate_buf, 0);
    } else if (strcmp(codec->name, "h264_qsv") == 0) {
        av_opt_set(ctx->priv_data, "b", rate_buf, 0);
        av_opt_set(ctx->priv_data, "maxrate", rate_buf, 0);
        av_opt_set(ctx->priv_data, "bufsize", rate_buf, 0);
    } else if (strcmp(codec->name, "h264_mf") == 0) {
        av_opt_set(ctx->priv_data, "b", rate_buf, 0);
        av_opt_set(ctx->priv_data, "maxrate", rate_buf, 0);
        av_opt_set(ctx->priv_data, "bufsize", rate_buf, 0);
    } else if (strcmp(codec->name, "libx264") == 0) {
        char kbps_buf[16] = {};
        snprintf(kbps_buf, sizeof(kbps_buf), "%d", bitrate_kb);
        av_opt_set(ctx->priv_data, "vbv-maxrate", kbps_buf, 0);
        av_opt_set(ctx->priv_data, "vbv-bufsize", kbps_buf, 0);
        av_opt_set(ctx->priv_data, "nal-hrd", "cbr", 0);
    }
}

bool ConfigureEncoderContext(
    AVCodecContext* ctx,
    const AVCodec* codec,
    int width,
    int height,
    int fps,
    int64_t bitrate_bps,
    int bitrate_kb,
    int rc_fps)
{
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{1, fps > 0 ? fps : 30};
    ctx->framerate = AVRational{fps > 0 ? fps : 30, 1};
    ctx->gop_size = fps > 0 ? fps : 30;
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (strcmp(codec->name, "h264_nvenc") == 0
        || strcmp(codec->name, "h264_amf") == 0
        || strcmp(codec->name, "h264_qsv") == 0
        || strcmp(codec->name, "h264_mf") == 0) {
        ctx->gop_size = std::min(ctx->gop_size, 120);
    }

    if (!ctx->priv_data) {
        ff::SetLastError(std::string(codec->name) + " encoder priv_data is null");
        return false;
    }

    if (strcmp(codec->name, "libx264") == 0) {
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    } else if (strcmp(codec->name, "h264_nvenc") == 0) {
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        av_opt_set(ctx->priv_data, "preset", "p1", 0);
        av_opt_set(ctx->priv_data, "tune", "ll", 0);
        av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
        av_opt_set(ctx->priv_data, "delay", "0", 0);
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
        av_opt_set(ctx->priv_data, "async_depth", "1", 0);
        {
            char fps_buf[16] = {};
            snprintf(fps_buf, sizeof(fps_buf), "%d", fps > 0 ? fps : 30);
            av_opt_set(ctx->priv_data, "fps", fps_buf, 0);
        }
    } else if (strcmp(codec->name, "h264_amf") == 0) {
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        av_opt_set(ctx->priv_data, "usage", "lowlatency", 0);
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
        av_opt_set(ctx->priv_data, "preanalysis", "0", 0);
        av_opt_set(ctx->priv_data, "header-insertion-mode", "none", 0);
        av_opt_set(ctx->priv_data, "vbaq", "0", 0);
        av_opt_set(ctx->priv_data, "frame_skipping", "0", 0);
        av_opt_set(ctx->priv_data, "enforce_hrd", "1", 0);
        {
            char fps_buf[16] = {};
            snprintf(fps_buf, sizeof(fps_buf), "%d", fps > 0 ? fps : 30);
            av_opt_set(ctx->priv_data, "framerate", fps_buf, 0);
        }
    } else if (strcmp(codec->name, "h264_qsv") == 0) {
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx->priv_data, "low_latency", "1", 0);
    } else if (strcmp(codec->name, "h264_mf") == 0) {
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        av_opt_set(ctx->priv_data, "rate_control", "cbr", 0);
        av_opt_set(ctx->priv_data, "scenario", "live_streaming", 0);
    } else {
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    const int eff_rc_fps = rc_fps > 0 ? rc_fps : (fps > 0 ? fps : 30);
    ApplyTargetBitrate(ctx, codec, bitrate_bps, bitrate_kb, eff_rc_fps);

    int ret = AVERROR_UNKNOWN;
    if (!FfSafeAvcodecOpen2(ctx, codec, &ret)) {
        ff::SetLastError(std::string(codec->name) + " avcodec_open2: hardware encoder crashed");
        return false;
    }
    if (ret < 0) {
        const std::string prefix = std::string(codec->name) + " avcodec_open2: ";
        SetAvError(prefix.c_str(), ret);
        return false;
    }
    return true;
}

void TuneEncoderRcFps(FfSession& session)
{
    if (session.realtime_fps < 20 || !session.enc_ctx || !session.enc_ctx->codec) {
        return;
    }

    const int cap = session.encoder_fps > 0 ? session.encoder_fps : 240;
    const int measured = std::min(session.realtime_fps, cap);
    if (measured <= 0 || measured < session.encoder_fps - 5) {
        return;
    }
    if (std::abs(measured - session.rc_fps) < 5) {
        return;
    }

    session.rc_fps = measured;
    AVCodecContext* ctx = session.enc_ctx;
    const AVCodec* codec = ctx->codec;
    const int64_t bps = ff::BitrateFromKbPerSec(session.bitrate_kb);

    ctx->framerate = AVRational{session.rc_fps, 1};
    char fps_buf[16] = {};
    snprintf(fps_buf, sizeof(fps_buf), "%d", session.rc_fps);
    if (strcmp(codec->name, "h264_amf") == 0) {
        av_opt_set(ctx->priv_data, "framerate", fps_buf, 0);
    } else if (strcmp(codec->name, "h264_nvenc") == 0) {
        av_opt_set(ctx->priv_data, "fps", fps_buf, 0);
    }

    ApplyTargetBitrate(ctx, codec, bps, session.bitrate_kb, session.rc_fps);
}

bool TryOpenEncoder(
    FfSession& session,
    const char* encoder_name,
    int enc_width,
    int enc_height,
    int fps,
    int64_t bitrate_bps)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        return false;
    }

    if (!ConfigureEncoderContext(
            ctx,
            codec,
            enc_width,
            enc_height,
            fps,
            bitrate_bps,
            session.bitrate_kb,
            session.rc_fps)) {
        FfSafeAvcodecFreeContext(&ctx);
        return false;
    }

    FfReleaseCodec(&session.enc_ctx);
    session.enc_ctx = ctx;
    session.encoder_type_gbk = BuildEncoderDescription(codec);
    return true;
}

bool OpenDecoder(AVCodecContext** dec_ctx, AVPixelFormat want_fmt = AV_PIX_FMT_YUV420P)
{
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!dec) {
        ff::SetLastError("H264 decoder not found");
        return false;
    }

    *dec_ctx = avcodec_alloc_context3(dec);
    if (!*dec_ctx) {
        ff::SetLastError("avcodec_alloc_context3 decoder failed");
        return false;
    }

    (*dec_ctx)->thread_count = 0;
    (*dec_ctx)->flags |= AV_CODEC_FLAG_LOW_DELAY;
    (*dec_ctx)->flags2 |= AV_CODEC_FLAG2_FAST;
    (*dec_ctx)->pix_fmt = want_fmt;

    if (avcodec_open2(*dec_ctx, dec, nullptr) < 0) {
        ff::SetLastError("avcodec_open2 decoder failed");
        avcodec_free_context(dec_ctx);
        return false;
    }
    return true;
}

void CopyBgraPitched(const uint8_t* src, int src_pitch, uint8_t* dst, int width, int height)
{
    const int dst_pitch = width * 4;
    const size_t row_bytes = static_cast<size_t>(dst_pitch);
    if (src_pitch == dst_pitch) {
        memcpy(dst, src, row_bytes * static_cast<size_t>(height));
        return;
    }
    for (int y = 0; y < height; ++y) {
        memcpy(
            dst + static_cast<size_t>(y) * row_bytes,
            src + static_cast<size_t>(y) * static_cast<size_t>(src_pitch),
            row_bytes);
    }
}

void FfCachePreviewBgra(FfSession& session, const uint8_t* bgra, int src_pitch)
{
    const int dst_pitch = session.src_width * 4;
    uint8_t* scratch = session.scratch_bgra.data();
    if (bgra == scratch && src_pitch == dst_pitch) {
        session.preview_bgra_valid = true;
        ++session.preview_serial;
        return;
    }
    CopyBgraPitched(bgra, src_pitch, scratch, session.src_width, session.src_height);
    session.preview_bgra_valid = true;
    ++session.preview_serial;
}

void FfMarkPreviewInScratch(FfSession& session)
{
    session.preview_bgra_valid = true;
    ++session.preview_serial;
    session.last_fed_preview_serial = session.preview_serial;
}

const uint8_t* StageBgraInScratch(
    FfSession& session,
    const uint8_t* bgra,
    int src_pitch,
    int capture_bytes)
{
    const int dst_pitch = session.src_width * 4;
    const int expected = session.render_bgra_size;
    if (!bgra || src_pitch < dst_pitch || expected <= 0) {
        ff::SetLastError("invalid BGRA stride");
        return nullptr;
    }
    if (capture_bytes > 0 && capture_bytes < expected) {
        ff::SetLastError("BGRA capture length too small");
        return nullptr;
    }

    uint8_t* scratch = session.scratch_bgra.data();
    if (bgra == scratch && src_pitch == dst_pitch) {
        return scratch;
    }

    if (src_pitch == dst_pitch) {
        memcpy(scratch, bgra, static_cast<size_t>(expected));
    } else {
        CopyBgraPitched(bgra, src_pitch, scratch, session.src_width, session.src_height);
    }
    return scratch;
}

int FinishBgraEncodeFrame(FfSession& session, int enc_ret)
{
    FfSessionOnFrameDone(session, session.last_encode_bytes);
    FfSessionThrottle(session, true);
    return enc_ret;
}

void FfStoreLastH264Packet(FfSession& session, const uint8_t* data, int len)
{
    if (!data || len <= 0) {
        return;
    }
    if (static_cast<int>(session.scratch_h264.size()) < len) {
        session.scratch_h264.resize(static_cast<size_t>(len));
    }
    memcpy(session.scratch_h264.data(), data, static_cast<size_t>(len));
    session.last_h264_stored = len;
}

void FlushPendingEncoderPackets(FfSession& session, int* out_bytes)
{
    int total = 0;
    for (;;) {
        av_packet_unref(session.enc_pkt);
        const int ret = avcodec_receive_packet(session.enc_ctx, session.enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        total += session.enc_pkt->size;
        av_packet_unref(session.enc_pkt);
    }
    if (out_bytes) {
        *out_bytes = total;
    }
}

int FinishEncoderAfterFeed(
    FfSession& session,
    uint8_t* h264_out,
    int h264_cap,
    int* in_out_len,
    bool copy_to_out,
    bool store_h264_for_decode)
{
    int enc_ret = 1;
    int total_bytes = 0;
    int out_offset = 0;
    int scratch_offset = 0;

    for (;;) {
        av_packet_unref(session.enc_pkt);
        const int ret = avcodec_receive_packet(session.enc_ctx, session.enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            SetAvError("avcodec_receive_packet: ", ret);
            return -1;
        }

        enc_ret = 0;
        const int pkt_len = session.enc_pkt->size;
        total_bytes += pkt_len;
        session.last_packet_preview_serial = session.preview_serial;

        if (copy_to_out && h264_out && pkt_len > 0) {
            if (out_offset + pkt_len > h264_cap) {
                ff::SetLastError("H264 output buffer too small");
                av_packet_unref(session.enc_pkt);
                return -2;
            }
            memcpy(
                h264_out + out_offset,
                session.enc_pkt->data,
                static_cast<size_t>(pkt_len));
            out_offset += pkt_len;
        }

        if (store_h264_for_decode && pkt_len > 0) {
            const int need = scratch_offset + pkt_len;
            if (static_cast<int>(session.scratch_h264.size()) < need) {
                session.scratch_h264.resize(static_cast<size_t>(need));
            }
            memcpy(
                session.scratch_h264.data() + static_cast<size_t>(scratch_offset),
                session.enc_pkt->data,
                static_cast<size_t>(pkt_len));
            scratch_offset += pkt_len;
        }

        av_packet_unref(session.enc_pkt);
    }

    session.last_encode_bytes = total_bytes;
    if (in_out_len) {
        *in_out_len = copy_to_out ? out_offset : total_bytes;
    }
    if (store_h264_for_decode && scratch_offset > 0) {
        session.last_h264_stored = scratch_offset;
    }

    if (enc_ret == 0) {
        ff::ClearLastError();
    }
    return enc_ret;
}

int ConvertFrameToBgra(
    FfSession& session,
    AVFrame* src,
    uint8_t* bgra_out,
    int bgra_capacity)
{
    if (bgra_capacity < session.render_bgra_size) {
        ff::SetLastError("render buffer too small");
        return -1;
    }

    int crop_x0 = 0;
    int crop_y0 = 0;
    int crop_x1 = src->width;
    int crop_y1 = src->height;
    if (session.content_rect_valid) {
        crop_x0 = session.content_x0;
        crop_y0 = session.content_y0;
        crop_x1 = session.content_x1;
        crop_y1 = session.content_y1;
    }

    int crop_w = crop_x1 - crop_x0;
    int crop_h = crop_y1 - crop_y0;
    if (crop_w < 32 || crop_h < 32) {
        crop_x0 = 0;
        crop_y0 = 0;
        crop_w = src->width;
        crop_h = src->height;
    }

    int fit_w = 0;
    int fit_h = 0;
    int off_x = 0;
    int off_y = 0;
    ComputeAspectFit(
        crop_w,
        crop_h,
        session.src_width,
        session.src_height,
        &fit_w,
        &fit_h,
        &off_x,
        &off_y);

    EnsureDecSws(session, src, crop_w, crop_h, fit_w, fit_h);
    if (!session.sws_dec) {
        ff::SetLastError("sws_getContext decode failed");
        return -1;
    }

    memset(bgra_out, 0, static_cast<size_t>(session.render_bgra_size));

    const uint8_t* src_slices[4] = {};
    int src_stride[4] = {};
    PrepareCroppedAvSrc(src, crop_x0, crop_y0, src_slices, src_stride);

    const int dst_linesize = session.src_width * 4;
    uint8_t* dst_base = bgra_out + static_cast<size_t>(off_y) * static_cast<size_t>(dst_linesize)
        + static_cast<size_t>(off_x) * 4;
    uint8_t* dst_slices[1] = {dst_base};
    int dst_stride_arr[1] = {dst_linesize};

    sws_scale(
        session.sws_dec,
        src_slices,
        src_stride,
        0,
        crop_h,
        dst_slices,
        dst_stride_arr);
    return 0;
}

bool FeedBgraToEncoderWithStride(
    FfSession& session,
    const uint8_t* bgra,
    int src_pitch,
    int capture_bytes,
    bool cache_preview)
{
    const int expected = session.src_width * session.src_height * 4;
    if (capture_bytes > 0 && capture_bytes < expected) {
        ff::SetLastError("BGRA capture length too small");
        return false;
    }
    if (!bgra || src_pitch < session.src_width * 4) {
        ff::SetLastError("invalid BGRA stride");
        return false;
    }

    if (!session.sws_enc) {
        if (!InitScaler(
                &session.sws_enc,
                session.src_width,
                session.src_height,
                AV_PIX_FMT_BGRA,
                session.enc_width,
                session.enc_height,
                session.enc_ctx->pix_fmt,
                SWS_POINT)) {
            ff::SetLastError("sws_getContext encode failed");
            return false;
        }
    }

    auto ensure_yuv_frame = [&](AVFrame** slot) -> bool {
        if (*slot) {
            return true;
        }
        *slot = av_frame_alloc();
        if (!*slot) {
            ff::SetLastError("av_frame_alloc enc_frame_yuv failed");
            return false;
        }
        (*slot)->format = session.enc_ctx->pix_fmt;
        (*slot)->width = session.enc_width;
        (*slot)->height = session.enc_height;
        if (av_frame_get_buffer(*slot, 32) < 0) {
            ff::SetLastError("av_frame_get_buffer enc_frame_yuv failed");
            return false;
        }
        return true;
    };

    if (!ensure_yuv_frame(&session.enc_frame_yuv) || !ensure_yuv_frame(&session.enc_frame_yuv_alt)) {
        return false;
    }

    AVFrame* yuv = (session.enc_yuv_write_idx == 0) ? session.enc_frame_yuv : session.enc_frame_yuv_alt;

    const uint8_t* src_slices[1] = {bgra};
    int src_stride[1] = {src_pitch};
    sws_scale(
        session.sws_enc,
        src_slices,
        src_stride,
        0,
        session.src_height,
        yuv->data,
        yuv->linesize);

    yuv->pts = session.enc_pts++;

    int ret = avcodec_send_frame(session.enc_ctx, yuv);
    if (ret == AVERROR(EAGAIN)) {
        FlushPendingEncoderPackets(session, nullptr);
        ret = avcodec_send_frame(session.enc_ctx, yuv);
    }
    if (ret < 0) {
        SetAvError("avcodec_send_frame: ", ret);
        return false;
    }
    session.enc_yuv_write_idx ^= 1;
    if (cache_preview) {
        FfCachePreviewBgra(session, bgra, src_pitch);
    }
    return true;
}

bool FeedBgraToEncoder(FfSession& session, const uint8_t* bgra, int capture_bytes)
{
    return FeedBgraToEncoderWithStride(session, bgra, session.src_width * 4, capture_bytes, true);
}

int EncodeBgraFrameImpl(
    FfSession& session,
    const uint8_t* bgra,
    int src_pitch,
    int capture_bytes,
    uint8_t* h264_out,
    int* in_out_len,
    bool copy_h264_out,
    bool /*store_h264_for_decode*/)
{
    if (!bgra) {
        ff::SetLastError("invalid encode parameters");
        return -1;
    }
    if (copy_h264_out && (!h264_out || !in_out_len || *in_out_len <= 0)) {
        ff::SetLastError("invalid encode parameters");
        return -1;
    }

    const int bytes_for_feed = capture_bytes > 0 ? capture_bytes : session.render_bgra_size;
    bool feed_ok = false;
    if (session.use_decode_preview) {
        const uint8_t* staged = StageBgraInScratch(session, bgra, src_pitch, bytes_for_feed);
        if (!staged) {
            return -1;
        }
        feed_ok = FeedBgraToEncoderWithStride(
            session,
            staged,
            session.src_width * 4,
            session.render_bgra_size,
            false);
        if (feed_ok) {
            FfMarkPreviewInScratch(session);
        }
    } else {
        feed_ok = FeedBgraToEncoderWithStride(session, bgra, src_pitch, bytes_for_feed, false);
    }
    if (!feed_ok) {
        return -1;
    }

    int dummy_len = 0;
    int* len_out = in_out_len ? in_out_len : &dummy_len;
    const int cap = (copy_h264_out && in_out_len) ? *in_out_len : 0;
    const int enc_ret = FinishEncoderAfterFeed(
        session,
        copy_h264_out ? h264_out : nullptr,
        cap,
        len_out,
        copy_h264_out,
        false);
    return FinishBgraEncodeFrame(session, enc_ret);
}

int EncodeBgraFramePitchedImpl(
    FfSession& session,
    const uint8_t* bgra,
    int src_pitch,
    int capture_bytes,
    uint8_t* h264_out,
    int* in_out_len)
{
    return EncodeBgraFrameImpl(
        session,
        bgra,
        src_pitch,
        capture_bytes,
        h264_out,
        in_out_len,
        true,
        true);
}

int EncodeD3D11TextureToH264(
    FfSession& session,
    int texture_ptr,
    uint8_t* h264_out,
    int* in_out_len)
{
    const uint8_t* mapped = nullptr;
    int pitch = 0;
    if (!FfBeginD3D11Read(session, texture_ptr, &mapped, &pitch)) {
        return -1;
    }

    FfCopyMappedBgraToScratch(session, mapped, pitch);
    FfEndD3D11Read(session);

    return EncodeBgraFramePitchedImpl(
        session,
        session.scratch_bgra.data(),
        session.src_width * 4,
        session.render_bgra_size,
        h264_out,
        in_out_len);
}

int ProcessFrameD3D11Impl(
    FfSession& session,
    int texture_ptr,
    uint8_t* render_buf,
    int render_capacity)
{
    if (!render_buf || render_capacity < session.render_bgra_size) {
        ff::SetLastError("render buffer too small");
        return -1;
    }

    int h264_len = session.h264_buffer_size;
    if (h264_len <= 0) {
        h264_len = static_cast<int>(session.scratch_h264.size());
    }
    if (h264_len <= 0) {
        ff::SetLastError("H264 buffer not configured");
        return -1;
    }

    const int enc_ret = EncodeD3D11TextureToH264(
        session,
        texture_ptr,
        session.scratch_h264.data(),
        &h264_len);
    if (enc_ret < 0) {
        return enc_ret;
    }

    if (enc_ret == 0 && session.last_encode_bytes > 0) {
        const int dec_ret = FfDecodeH264ToBgra(
            session,
            session.scratch_h264.data(),
            session.last_encode_bytes,
            render_buf,
            render_capacity);
        if (dec_ret < 0) {
            return dec_ret;
        }
        return dec_ret == 0 ? 0 : 1;
    }

    if (session.preview_bgra_valid) {
        memcpy(
            render_buf,
            session.scratch_bgra.data(),
            static_cast<size_t>(session.render_bgra_size));
        ff::ClearLastError();
    }
    return enc_ret;
}

} // namespace

void FfCopyMappedBgraToScratch(FfSession& session, const uint8_t* mapped, int pitch)
{
    const int dst_pitch = session.src_width * 4;
    const size_t row_bytes = static_cast<size_t>(dst_pitch);
    uint8_t* dst = session.scratch_bgra.data();
    if (pitch == dst_pitch) {
        memcpy(dst, mapped, row_bytes * static_cast<size_t>(session.src_height));
        return;
    }
    for (int y = 0; y < session.src_height; ++y) {
        memcpy(
            dst + static_cast<size_t>(y) * row_bytes,
            mapped + static_cast<size_t>(y) * static_cast<size_t>(pitch),
            row_bytes);
    }
}

FfSessionManager& FfSessionManager::Instance()
{
    static FfSessionManager inst;
    return inst;
}

int FfSessionManager::Create(std::unique_ptr<FfSession> session)
{
    FfCsLock lock(map_mutex_);
    const int handle = next_handle_.fetch_add(1);
    sessions_[handle] = std::move(session);
    return handle;
}

FfSessionManager::SessionGuard FfSessionManager::Acquire(int handle)
{
    auto map_lock = std::make_unique<FfCsLock>(map_mutex_);
    const auto it = sessions_.find(handle);
    if (it == sessions_.end()) {
        ff::SetLastError("invalid session handle");
        return SessionGuard(nullptr, nullptr, nullptr);
    }

    FfSession* session = it->second.get();
    auto session_lock = std::make_unique<FfCsLock>(session->mutex);
    return SessionGuard(std::move(map_lock), std::move(session_lock), session);
}

std::unique_ptr<FfSession> FfSessionManager::Remove(int handle)
{
    FfCsLock lock(map_mutex_);
    const auto it = sessions_.find(handle);
    if (it == sessions_.end()) {
        return nullptr;
    }
    auto session = std::move(it->second);
    sessions_.erase(it);
    return session;
}

void FfReleaseCodec(AVCodecContext** ctx)
{
    if (ctx && *ctx) {
        avcodec_free_context(ctx);
        *ctx = nullptr;
    }
}

void FfReleaseFrame(AVFrame** frame)
{
    if (frame && *frame) {
        av_frame_free(frame);
        *frame = nullptr;
    }
}

void FfReleasePacket(AVPacket** pkt)
{
    if (pkt && *pkt) {
        av_packet_free(pkt);
        *pkt = nullptr;
    }
}

void FfReleaseSws(SwsContext** sws)
{
    if (sws && *sws) {
        sws_freeContext(*sws);
        *sws = nullptr;
    }
}

void FfEndD3D11Read(FfSession& session)
{
    if (!session.d3d_mapped) {
        return;
    }
    auto* context = static_cast<ID3D11DeviceContext*>(session.d3d_context);
    auto* staging = static_cast<ID3D11Texture2D*>(session.d3d_staging);
    if (context && staging) {
        context->Unmap(staging, 0);
    }
    session.d3d_mapped = false;
}

void ReleaseD3D11Cache(FfSession& session)
{
    FfEndD3D11Read(session);
    if (session.d3d_staging) {
        static_cast<ID3D11Texture2D*>(session.d3d_staging)->Release();
        session.d3d_staging = nullptr;
    }
    if (session.d3d_context) {
        static_cast<ID3D11DeviceContext*>(session.d3d_context)->Release();
        session.d3d_context = nullptr;
    }
    if (session.d3d_device) {
        static_cast<ID3D11Device*>(session.d3d_device)->Release();
        session.d3d_device = nullptr;
    }
    session.d3d_staging_w = 0;
    session.d3d_staging_h = 0;
    session.d3d_staging_format = 0;
}

void FlushCodecPackets(AVCodecContext* ctx, AVPacket* pkt)
{
    if (!ctx || !pkt) {
        return;
    }

    avcodec_send_frame(ctx, nullptr);
    for (;;) {
        av_packet_unref(pkt);
        const int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        av_packet_unref(pkt);
    }
}

void FfSessionDestroy(FfSession& session)
{
    FfPlayClose(session);
    FfRecordEnd(session);
    FfEndD3D11Read(session);
    ReleaseD3D11Cache(session);

    if (session.enc_ctx && session.enc_pkt) {
        FlushCodecPackets(session.enc_ctx, session.enc_pkt);
    }

    FfReleaseSws(&session.sws_enc);
    FfReleaseSws(&session.sws_dec);
    FfReleaseFrame(&session.enc_frame_yuv);
    FfReleaseFrame(&session.enc_frame_yuv_alt);
    FfReleaseFrame(&session.enc_frame_bgra);
    FfReleaseFrame(&session.dec_frame);
    FfReleaseFrame(&session.dec_frame_bgra);
    FfReleasePacket(&session.enc_pkt);
    FfReleasePacket(&session.dec_pkt);
    FfReleaseCodec(&session.enc_ctx);
    FfReleaseCodec(&session.dec_ctx);

    session.pending_h264.clear();
    session.scratch_bgra.clear();
    session.scratch_h264.clear();
    session.preview_bgra_valid = false;
    session.use_decode_preview = false;
    session.d3d_mapped = false;
    session.pace_deadline_active = false;
    FfOrientReset(session);
}

void FfSessionOnFrameDone(FfSession& session, int encoded_bytes)
{
    const auto now = std::chrono::steady_clock::now();
    if (session.stats_frame_count == 0) {
        session.stats_start = now;
    }

    ++session.stats_frame_count;
    if (encoded_bytes > 0) {
        session.stats_byte_count += encoded_bytes;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session.stats_start).count();
    if (elapsed >= 500) {
        session.realtime_fps = static_cast<int>((session.stats_frame_count * 1000LL) / std::max<int64_t>(elapsed, 1));
        session.realtime_bitrate_kb = static_cast<int>(
            (session.stats_byte_count * 1000LL) / (1024LL * std::max<int64_t>(elapsed, 1)));
        session.stats_start = now;
        session.stats_frame_count = 0;
        session.stats_byte_count = 0;
        TuneEncoderRcFps(session);
    }
}

void FfSessionThrottle(FfSession& session, bool enable)
{
    if (!enable || session.target_fps <= 0) {
        return;
    }

    const auto interval = std::chrono::microseconds(1000000LL / session.target_fps);
    const auto now = std::chrono::steady_clock::now();

    if (!session.pace_deadline_active) {
        session.pace_next_deadline = now;
        session.pace_deadline_active = true;
    }

    session.pace_next_deadline += interval;

    if (now < session.pace_next_deadline) {
        const auto wait = session.pace_next_deadline - now;
        const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(wait).count();
        if (wait_us > 2000) {
            const DWORD sleep_ms = static_cast<DWORD>((wait_us - 1000) / 1000);
            if (sleep_ms > 0) {
                Sleep(sleep_ms);
            }
        }
        while (std::chrono::steady_clock::now() < session.pace_next_deadline) {
            std::this_thread::yield();
        }
        return;
    }

    const auto behind = now - session.pace_next_deadline;
    if (behind > interval * 5) {
        session.pace_next_deadline = now;
    }
}

bool FfSessionInit(FfSession& session, int use_cpu, int width, int height, int fps, int bitrate_kb)
{
    if (width <= 0 || height <= 0) {
        ff::SetLastError("invalid width/height");
        return false;
    }

    session.src_width = width;
    session.src_height = height;
    session.encoder_fps = fps > 0 ? fps : 30;
    session.rc_fps = session.encoder_fps;
    session.target_fps = fps > 0 ? fps : 0;
    session.enc_scale_denom = 1;
    session.bitrate_kb = bitrate_kb > 0 ? bitrate_kb : 2500;
    if (session.encoder_fps >= 90
        && width * height >= 1280 * 720
        && session.bitrate_kb < 4000) {
        session.enc_scale_denom = 2;
    }
    session.enc_width = ff::AlignEven(width / session.enc_scale_denom);
    session.enc_height = ff::AlignEven(height / session.enc_scale_denom);
    if (!ValidateInitBufferSizes(
            session.src_width,
            session.src_height,
            session.enc_width,
            session.enc_height)) {
        ff::SetLastError("invalid width/height or buffer size overflow");
        return false;
    }

    session.use_cpu_only = (use_cpu != 0);

    // N卡 -> A卡 -> Intel核显 -> Windows MF -> CPU(libx264)
    static const char* kHwEncoders[] = {"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"};
    const int64_t bitrate = ff::BitrateFromKbPerSec(session.bitrate_kb);
    bool opened = false;

    if (!session.use_cpu_only) {
        for (const char* name : kHwEncoders) {
            if (TryOpenEncoder(
                    session,
                    name,
                    session.enc_width,
                    session.enc_height,
                    session.encoder_fps,
                    bitrate)) {
                opened = true;
                break;
            }
        }
    }

    if (!opened) {
        if (!TryOpenEncoder(
                session,
                "libx264",
                session.enc_width,
                session.enc_height,
                session.encoder_fps,
                bitrate)) {
            ff::SetLastError("no usable H264 encoder (hardware and libx264 failed)");
            FfSessionDestroy(session);
            return false;
        }
    }

    session.render_bgra_size = session.src_width * 4 * session.src_height;
    session.h264_buffer_size = session.enc_width * session.enc_height * 2;
    session.scratch_bgra.resize(static_cast<size_t>(session.render_bgra_size));
    session.scratch_h264.resize(static_cast<size_t>(session.h264_buffer_size));

    session.enc_pkt = av_packet_alloc();
    if (!session.enc_pkt) {
        ff::SetLastError("alloc enc packet failed");
        FfSessionDestroy(session);
        return false;
    }

    session.stats_start = {};
    session.pace_next_deadline = {};
    session.pace_deadline_active = false;
    session.stats_frame_count = 0;
    session.stats_byte_count = 0;
    session.realtime_fps = 0;
    session.realtime_bitrate_kb = 0;
    session.enc_pts = 0;
    session.enc_yuv_write_idx = 0;
    session.last_encode_bytes = 0;
    session.last_h264_stored = 0;
    session.preview_bgra_valid = false;
    session.use_decode_preview = false;
    session.preview_serial = 0;
    session.last_fed_preview_serial = 0;
    session.last_packet_preview_serial = 0;
    session.pending_h264.clear();
    session.dec_need_more = false;

    ff::ClearLastError();
    return true;
}

bool FfEnsureDecoder(FfSession& session)
{
    if (session.dec_ctx) {
        return true;
    }

    if (!OpenDecoder(&session.dec_ctx)) {
        FfReleaseCodec(&session.dec_ctx);
        return false;
    }

    session.dec_pkt = av_packet_alloc();
    session.dec_frame = av_frame_alloc();
    if (!session.dec_pkt || !session.dec_frame) {
        ff::SetLastError("alloc dec packet/frame failed");
        FfReleasePacket(&session.dec_pkt);
        FfReleaseFrame(&session.dec_frame);
        FfReleaseCodec(&session.dec_ctx);
        return false;
    }

    ff::ClearLastError();
    return true;
}

bool FfBeginD3D11Read(FfSession& session, int texture_ptr, const uint8_t** out_data, int* out_pitch)
{
    if (!out_data || !out_pitch) {
        ff::SetLastError("invalid D3D read parameters");
        return false;
    }
    if (texture_ptr == 0) {
        ff::SetLastError("D3D texture pointer is null");
        return false;
    }

    FfEndD3D11Read(session);

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(static_cast<intptr_t>(texture_ptr));
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    ID3D11Device* src_device = nullptr;
    texture->GetDevice(&src_device);
    if (!src_device) {
        ff::SetLastError("GetDevice from texture failed");
        return false;
    }

    auto* cached_device = static_cast<ID3D11Device*>(session.d3d_device);
    if (!cached_device) {
        src_device->AddRef();
        session.d3d_device = src_device;
        ID3D11DeviceContext* ctx = nullptr;
        src_device->GetImmediateContext(&ctx);
        session.d3d_context = ctx;
    } else if (cached_device != src_device) {
        ReleaseD3D11Cache(session);
        src_device->AddRef();
        session.d3d_device = src_device;
        ID3D11DeviceContext* ctx = nullptr;
        src_device->GetImmediateContext(&ctx);
        session.d3d_context = ctx;
    }
    src_device->Release();

    auto* device = static_cast<ID3D11Device*>(session.d3d_device);
    auto* context = static_cast<ID3D11DeviceContext*>(session.d3d_context);
    if (!context) {
        ff::SetLastError("GetImmediateContext failed");
        return false;
    }

    auto* staging = static_cast<ID3D11Texture2D*>(session.d3d_staging);
    const bool need_staging = !staging
        || session.d3d_staging_w != static_cast<int>(desc.Width)
        || session.d3d_staging_h != static_cast<int>(desc.Height)
        || session.d3d_staging_format != static_cast<unsigned>(desc.Format);

    if (need_staging) {
        if (staging) {
            staging->Release();
            session.d3d_staging = nullptr;
            staging = nullptr;
        }

        D3D11_TEXTURE2D_DESC staging_desc = desc;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.MiscFlags = 0;

        ID3D11Texture2D* new_staging = nullptr;
        if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &new_staging)) || !new_staging) {
            ff::SetLastError("CreateTexture2D staging failed");
            return false;
        }
        session.d3d_staging = new_staging;
        staging = new_staging;
        session.d3d_staging_w = static_cast<int>(desc.Width);
        session.d3d_staging_h = static_cast<int>(desc.Height);
        session.d3d_staging_format = static_cast<unsigned>(desc.Format);
    }

    context->CopyResource(staging, texture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ff::SetLastError("Map staging texture failed");
        return false;
    }

    *out_data = static_cast<const uint8_t*>(mapped.pData);
    *out_pitch = static_cast<int>(mapped.RowPitch);
    session.d3d_mapped = true;
    return true;
}

int FfCopyD3D11TextureToBgra(FfSession& session, int texture_ptr, std::vector<uint8_t>& out)
{
    const uint8_t* mapped = nullptr;
    int pitch = 0;
    if (!FfBeginD3D11Read(session, texture_ptr, &mapped, &pitch)) {
        return -1;
    }

    const size_t needed = static_cast<size_t>(session.src_width) * static_cast<size_t>(session.src_height) * 4;
    out.resize(needed);
    FfCopyMappedBgraToScratch(session, mapped, pitch);
    memcpy(out.data(), session.scratch_bgra.data(), needed);
    FfEndD3D11Read(session);
    return 0;
}

int FfEncodeFrameFromD3D11Texture(
    FfSession& session,
    int texture_ptr,
    uint8_t* h264_out,
    int* in_out_len)
{
    return EncodeD3D11TextureToH264(session, texture_ptr, h264_out, in_out_len);
}

int FfProcessFrameD3D11(
    FfSession& session,
    int texture_ptr,
    uint8_t* render_buf,
    int render_capacity)
{
    return ProcessFrameD3D11Impl(session, texture_ptr, render_buf, render_capacity);
}

int FfEncodeBgraFrame(FfSession& session, const uint8_t* bgra, int capture_bytes, uint8_t* h264_out, int* in_out_len)
{
    return EncodeBgraFramePitchedImpl(
        session,
        bgra,
        session.src_width * 4,
        capture_bytes,
        h264_out,
        in_out_len);
}

int FfEncodeBgraFramePitched(
    FfSession& session,
    const uint8_t* bgra,
    int src_pitch,
    int capture_bytes,
    uint8_t* h264_out,
    int* in_out_len)
{
    return EncodeBgraFramePitchedImpl(session, bgra, src_pitch, capture_bytes, h264_out, in_out_len);
}

int FfDecodeH264ToBgra(
    FfSession& session,
    const uint8_t* h264,
    int h264_len,
    uint8_t* bgra_out,
    int bgra_capacity)
{
    session.use_decode_preview = true;

    if (!h264 || h264_len <= 0 || !bgra_out || bgra_capacity <= 0) {
        ff::SetLastError("invalid decode parameters");
        return -1;
    }

    if (session.preview_bgra_valid
        && session.preview_serial == session.last_fed_preview_serial
        && bgra_capacity >= session.render_bgra_size) {
        memcpy(bgra_out, session.scratch_bgra.data(), static_cast<size_t>(session.render_bgra_size));
        if (!session.dec_frame_dims_valid) {
            session.dec_frame_w = session.src_width;
            session.dec_frame_h = session.src_height;
            session.dec_frame_dims_valid = true;
        }
        FfOrientUpdateFromBgra(
            session,
            bgra_out,
            session.src_width,
            session.src_height,
            session.src_width * 4,
            bgra_capacity);
        ff::ClearLastError();
        return 0;
    }

    if (!FfEnsureDecoder(session)) {
        return -1;
    }

    av_packet_unref(session.dec_pkt);
    session.dec_pkt->data = const_cast<uint8_t*>(h264);
    session.dec_pkt->size = h264_len;
    session.dec_pkt->buf = nullptr;

    int ret = avcodec_send_packet(session.dec_ctx, session.dec_pkt);
    av_packet_unref(session.dec_pkt);
    if (ret < 0) {
        SetAvError("avcodec_send_packet decode: ", ret);
        return -1;
    }

    ret = avcodec_receive_frame(session.dec_ctx, session.dec_frame);
    if (ret == AVERROR(EAGAIN)) {
        return 1;
    }
    if (ret == AVERROR_EOF) {
        return 1;
    }
    if (ret < 0) {
        SetAvError("avcodec_receive_frame: ", ret);
        return -1;
    }

    session.dec_frame_w = session.dec_frame->width;
    session.dec_frame_h = session.dec_frame->height;
    session.dec_frame_dims_valid = session.dec_frame_w > 0 && session.dec_frame_h > 0;

    FfOrientUpdateFromAvFrame(session, session.dec_frame);

    if (ConvertFrameToBgra(session, session.dec_frame, bgra_out, bgra_capacity) < 0) {
        return -1;
    }

    ff::ClearLastError();
    return 0;
}
