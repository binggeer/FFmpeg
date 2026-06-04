#include "pch.h"
#include "ff_play_wasapi.h"
#include "ff_session.h"
#include "ff_util.h"

#include <algorithm>

namespace {

const char* AvCodecIdName(AVCodecID id)
{
    return avcodec_get_name(id);
}

void SetAvError(const char* prefix, int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    ff::SetLastError(std::string(prefix) + buf);
}

void ReleasePlayState(FfSession::PlayState& play)
{
    FfReleaseSws(&play.sws);
    if (play.audio_swr) {
        swr_free(&play.audio_swr);
        play.audio_swr = nullptr;
    }
    FfReleaseFrame(&play.frame);
    FfReleaseFrame(&play.audio_frame);
    FfReleaseFrame(&play.frame_bgra);
    FfReleasePacket(&play.pkt);
    FfReleaseCodec(&play.dec);
    FfReleaseCodec(&play.audio_dec);
    if (play.wasapi_play) {
        FfPlayWasapiClose(static_cast<WasapiPlayback*>(play.wasapi_play));
        play.wasapi_play = nullptr;
    }
    play.audio_pcm_scratch.clear();
    if (play.fmt) {
        avformat_close_input(&play.fmt);
        play.fmt = nullptr;
    }
    play.sws_src_w = 0;
    play.sws_src_h = 0;
    play.sws_src_fmt = -1;
    play.video_stream = -1;
    play.audio_stream = -1;
    play.width = 0;
    play.height = 0;
    play.active = false;
    play.has_audio = false;
    play.audio_device_rate = 0;
    play.audio_device_channels = 0;
    play.paused = false;
    play.pause_started_wall_ms = 0;
    play.last_pts = 0;
    play.pace_frame_duration_ms = 40;
    play.pace_started = false;
    play.pace_base_pts_ms = 0;
    play.pace_base_wall_ms = 0;
    play.pace_last_pts_ms = -1;
    play.pace_no_pts_index = 0;
}

void ResetPlayPace(FfSession::PlayState& play)
{
    play.pace_started = false;
    play.pace_base_pts_ms = 0;
    play.pace_base_wall_ms = 0;
    play.pace_last_pts_ms = -1;
    play.pace_no_pts_index = 0;
}

int64_t StreamPtsToMs(const AVStream* stream, int64_t pts)
{
    if (!stream || pts == AV_NOPTS_VALUE) {
        return -1;
    }
    return av_rescale_q(pts, stream->time_base, AVRational{1, 1000});
}

int64_t StreamFrameDurationMs(const AVStream* stream)
{
    if (!stream) {
        return 40;
    }

    AVRational fps = stream->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) {
        fps = stream->r_frame_rate;
    }
    if (fps.num <= 0 || fps.den <= 0) {
        return 40;
    }

    const int64_t frame_ms = av_rescale_q(1, av_inv_q(fps), AVRational{1, 1000});
    return frame_ms > 0 ? frame_ms : 40;
}

void WaitPlayPace(FfSession::PlayState& play, const AVStream* stream, int64_t pts)
{
    const int64_t frame_ms = play.pace_frame_duration_ms > 0 ? play.pace_frame_duration_ms : 40;
    int64_t pts_ms = StreamPtsToMs(stream, pts);
    if (pts_ms < 0) {
        if (!play.pace_started) {
            play.pace_started = true;
            play.pace_base_wall_ms = GetTickCount64();
            play.pace_no_pts_index = 0;
            return;
        }
        pts_ms = static_cast<int64_t>(play.pace_no_pts_index) * frame_ms;
        ++play.pace_no_pts_index;
    } else {
        if (play.pace_last_pts_ms >= 0 && pts_ms <= play.pace_last_pts_ms) {
            pts_ms = play.pace_last_pts_ms + frame_ms;
        }
        play.pace_last_pts_ms = pts_ms;
    }

    if (!play.pace_started) {
        play.pace_started = true;
        play.pace_base_pts_ms = pts_ms;
        play.pace_base_wall_ms = GetTickCount64();
        return;
    }

    const int64_t target_wall = play.pace_base_wall_ms + (pts_ms - play.pace_base_pts_ms);
    const int64_t now = GetTickCount64();
    if (target_wall > now) {
        const int64_t delay = target_wall - now;
        const DWORD sleep_ms = static_cast<DWORD>(std::min<int64_t>(delay, 60000));
        if (sleep_ms > 0) {
            Sleep(sleep_ms);
        }
    }
}

bool EnsurePlaySws(FfSession::PlayState& play, AVFrame* src, int dst_w, int dst_h)
{
    if (src->width <= 0 || src->height <= 0) {
        ff::SetLastError("play decoded frame has invalid size");
        return false;
    }

    const int src_fmt = src->format;
    if (play.sws
        && play.sws_src_w == src->width
        && play.sws_src_h == src->height
        && play.sws_src_fmt == src_fmt) {
        return true;
    }

    FfReleaseSws(&play.sws);
    play.sws = sws_getContext(
        src->width,
        src->height,
        static_cast<AVPixelFormat>(src_fmt),
        dst_w,
        dst_h,
        AV_PIX_FMT_BGRA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (!play.sws) {
        ff::SetLastError("play sws_getContext failed");
        return false;
    }
    play.sws_src_w = src->width;
    play.sws_src_h = src->height;
    play.sws_src_fmt = src_fmt;
    return true;
}

bool EnsurePlayBgraFrame(FfSession::PlayState& play, int width, int height)
{
    if (width <= 0 || height <= 0) {
        ff::SetLastError("play output size invalid");
        return false;
    }

    if (play.frame_bgra
        && play.frame_bgra->width == width
        && play.frame_bgra->height == height) {
        return true;
    }

    FfReleaseFrame(&play.frame_bgra);

    play.frame_bgra = av_frame_alloc();
    if (!play.frame_bgra) {
        ff::SetLastError("play av_frame_alloc bgra failed");
        return false;
    }
    play.frame_bgra->format = AV_PIX_FMT_BGRA;
    play.frame_bgra->width = width;
    play.frame_bgra->height = height;
    if (av_frame_get_buffer(play.frame_bgra, 32) < 0) {
        ff::SetLastError("play av_frame_get_buffer bgra failed");
        FfReleaseFrame(&play.frame_bgra);
        return false;
    }
    return true;
}

int CopyPlayFrameToBuffer(FfSession& session, FfSession::PlayState& play, AVFrame* src, uint8_t* out, int out_cap)
{
    const int dst_w = session.src_width;
    const int dst_h = session.src_height;
    const int needed = dst_w * 4 * dst_h;
    if (out_cap < needed || !out) {
        ff::SetLastError("play output buffer too small");
        return -1;
    }

    if (!EnsurePlayBgraFrame(play, dst_w, dst_h) || !EnsurePlaySws(play, src, dst_w, dst_h)) {
        return -1;
    }

    if (!play.sws || !src->data[0] || !play.frame_bgra || !play.frame_bgra->data[0]) {
        ff::SetLastError("play scaler or frame buffer missing");
        return -1;
    }

    sws_scale(
        play.sws,
        src->data,
        src->linesize,
        0,
        src->height,
        play.frame_bgra->data,
        play.frame_bgra->linesize);

    const int ret = av_image_copy_to_buffer(
        out,
        out_cap,
        play.frame_bgra->data,
        play.frame_bgra->linesize,
        AV_PIX_FMT_BGRA,
        dst_w,
        dst_h,
        1);
    if (ret < 0) {
        ff::SetLastError("play av_image_copy_to_buffer failed");
        return -1;
    }
    return needed;
}

bool PlayDecodedAudioFrame(FfSession::PlayState& play)
{
    if (!play.has_audio || !play.audio_dec || !play.audio_frame || !play.wasapi_play || !play.audio_swr) {
        return true;
    }

    auto* wp = static_cast<WasapiPlayback*>(play.wasapi_play);
    const int device_ch = play.audio_device_channels > 0 ? play.audio_device_channels : 2;

    const int in_samples = play.audio_frame->nb_samples;
    if (in_samples <= 0) {
        return true;
    }

    const int max_out = swr_get_out_samples(play.audio_swr, in_samples);
    const int out_cap_samples = std::max(in_samples, max_out) + 256;
    play.audio_pcm_scratch.resize(static_cast<size_t>(out_cap_samples) * static_cast<size_t>(device_ch) * sizeof(int16_t));

    uint8_t* out_data = play.audio_pcm_scratch.data();
    const int converted = swr_convert(
        play.audio_swr,
        &out_data,
        out_cap_samples,
        const_cast<const uint8_t**>(play.audio_frame->data),
        in_samples);
    if (converted < 0) {
        ff::SetLastError("play audio swr_convert failed");
        return false;
    }
    if (converted == 0) {
        return true;
    }

    const int out_channels = device_ch;
    if (!FfPlayWasapiWritePcm(wp, play.audio_pcm_scratch.data(), converted, out_channels)) {
        return false;
    }
    return true;
}

void DrainDecodedAudio(FfSession::PlayState& play)
{
    if (!play.has_audio || !play.audio_dec || !play.audio_frame) {
        return;
    }

    while (true) {
        const int ret = avcodec_receive_frame(play.audio_dec, play.audio_frame);
        if (ret == 0) {
            if (!PlayDecodedAudioFrame(play)) {
                return;
            }
            av_frame_unref(play.audio_frame);
            continue;
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        SetAvError("play audio avcodec_receive_frame: ", ret);
        return;
    }
}

bool DeliverDecodedVideoFrame(FfSession& session, FfSession::PlayState& play, uint8_t* bgra_out, int out_cap, int* out_len)
{
    play.last_pts = play.frame->best_effort_timestamp;
    if (play.last_pts == AV_NOPTS_VALUE) {
        play.last_pts = play.frame->pts;
    }

    /* 仅视频节流；音频不写 Sleep，避免与视频双重等待导致变慢、发飘 */
    if (play.fmt && play.video_stream >= 0) {
        WaitPlayPace(play, play.fmt->streams[play.video_stream], play.last_pts);
    }

    const int written = CopyPlayFrameToBuffer(session, play, play.frame, bgra_out, out_cap);
    if (written < 0) {
        return false;
    }

    *out_len = written;
    ff::ClearLastError();
    return true;
}

bool OpenPlayAudio(FfSession::PlayState& play)
{
    const int audio_find_ret = av_find_best_stream(play.fmt, AVMEDIA_TYPE_AUDIO, -1, play.video_stream, nullptr, 0);
    play.audio_stream = audio_find_ret;
    if (audio_find_ret < 0) {
        play.audio_stream = -1;
        play.has_audio = false;

        return true;
    }

    AVStream* astream = play.fmt->streams[play.audio_stream];


    const AVCodec* adec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (!adec) {
        ff::SetLastError("play audio decoder not found");
        return false;
    }

    play.audio_dec = avcodec_alloc_context3(adec);
    if (!play.audio_dec) {
        ff::SetLastError("play audio avcodec_alloc_context3 failed");
        return false;
    }

    if (avcodec_parameters_to_context(play.audio_dec, astream->codecpar) < 0) {
        ff::SetLastError("play audio avcodec_parameters_to_context failed");
        return false;
    }

    if (avcodec_open2(play.audio_dec, adec, nullptr) < 0) {
        ff::SetLastError("play audio avcodec_open2 failed");
        return false;
    }

    WasapiPlayback* wasapi = nullptr;
    int device_rate = 0;
    int device_ch = 0;
    if (!FfPlayWasapiOpen(&wasapi, &device_rate, &device_ch)) {
        FfReleaseCodec(&play.audio_dec);
        play.audio_dec = nullptr;
        play.audio_stream = -1;
        play.has_audio = false;
        ff::ClearLastError();
        return true;
    }

    play.wasapi_play = wasapi;
    play.audio_device_rate = device_rate;
    play.audio_device_channels = device_ch;
    FfPlayWasapiSetVolume(wasapi, play.volume_percent);

    play.audio_frame = av_frame_alloc();
    if (!play.audio_frame) {
        ff::SetLastError("play audio frame alloc failed");
        return false;
    }

    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, device_ch);

    const int swr_ret = swr_alloc_set_opts2(
        &play.audio_swr,
        &out_layout,
        AV_SAMPLE_FMT_S16,
        device_rate,
        &play.audio_dec->ch_layout,
        play.audio_dec->sample_fmt,
        play.audio_dec->sample_rate,
        0,
        nullptr);
    av_channel_layout_uninit(&out_layout);

    if (swr_ret < 0 || !play.audio_swr || swr_init(play.audio_swr) < 0) {
        ff::SetLastError("play audio swr_init failed");
        return false;
    }

    play.has_audio = true;

    return true;
}

} // namespace

int FfPlayOpen(FfSession& session, const char* path_gbk, int play_audio)
{
    if (!path_gbk || !path_gbk[0]) {
        ff::SetLastError("play path empty");
        return -1;
    }

    if (session.src_width <= 0 || session.src_height <= 0 || session.render_bgra_size <= 0) {
        ff::SetLastError("session width/height invalid, call FF_Init first");
        return -1;
    }

    FfPlayClose(session);

    const std::string path_utf8 = ff::GbkToUtf8(path_gbk);
    auto& play = session.play;

    if (avformat_open_input(&play.fmt, path_utf8.c_str(), nullptr, nullptr) < 0) {
        ff::SetLastError("avformat_open_input failed");
        return -1;
    }

    if (avformat_find_stream_info(play.fmt, nullptr) < 0) {
        ff::SetLastError("avformat_find_stream_info failed");
        ReleasePlayState(play);
        return -1;
    }


    play.video_stream = av_find_best_stream(play.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (play.video_stream < 0) {
        ff::SetLastError("video stream not found");
        ReleasePlayState(play);
        return -1;
    }

    AVStream* stream = play.fmt->streams[play.video_stream];
    const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) {
        ff::SetLastError("video decoder not found");
        ReleasePlayState(play);
        return -1;
    }

    play.dec = avcodec_alloc_context3(dec);
    if (!play.dec) {
        ff::SetLastError("play avcodec_alloc_context3 failed");
        ReleasePlayState(play);
        return -1;
    }

    if (avcodec_parameters_to_context(play.dec, stream->codecpar) < 0) {
        ff::SetLastError("avcodec_parameters_to_context failed");
        ReleasePlayState(play);
        return -1;
    }

    play.dec->thread_count = 0;
    play.dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(play.dec, dec, nullptr) < 0) {
        ff::SetLastError("play avcodec_open2 failed");
        ReleasePlayState(play);
        return -1;
    }

    if (play_audio != 0) {
        if (!OpenPlayAudio(play)) {
            ReleasePlayState(play);
            return -1;
        }
    }

    play.frame = av_frame_alloc();
    play.pkt = av_packet_alloc();
    if (!play.frame || !play.pkt) {
        ff::SetLastError("play frame/packet alloc failed");
        ReleasePlayState(play);
        return -1;
    }

    play.width = play.dec->width;
    play.height = play.dec->height;
    play.active = true;
    play.paused = false;
    play.pace_frame_duration_ms = StreamFrameDurationMs(stream);
    ResetPlayPace(play);

    avformat_seek_file(play.fmt, -1, 0, 0, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(play.dec);
    if (play.audio_dec) {
        avcodec_flush_buffers(play.audio_dec);
    }
    if (play.wasapi_play) {
        FfPlayWasapiReset(static_cast<WasapiPlayback*>(play.wasapi_play));
    }

    ff::ClearLastError();
    return 0;
}

int FfPlayReadBgra(FfSession& session, uint8_t* bgra_out, int* in_out_len)
{
    if (!session.play.active || !session.play.fmt || !session.play.dec || !session.play.frame || !session.play.pkt) {
        ff::SetLastError("play not active");
        return -1;
    }
    if (!bgra_out || !in_out_len) {
        ff::SetLastError("play invalid output buffer");
        return -1;
    }

    const int out_cap = *in_out_len;
    if (out_cap < session.render_bgra_size) {
        ff::SetLastError("play output buffer too small");
        return -1;
    }

    if (session.play.paused) {
        Sleep(10);
        return 2;
    }

    auto& play = session.play;

    int loop_guard = 0;
    while (true) {
        DrainDecodedAudio(play);

        const int frame_ret = avcodec_receive_frame(play.dec, play.frame);
        if (frame_ret == 0) {
            int written = 0;
            if (DeliverDecodedVideoFrame(session, play, bgra_out, out_cap, &written)) {
                av_frame_unref(play.frame);
                *in_out_len = written;
                return 0;
            }
            av_frame_unref(play.frame);
            return -1;
        }

        if (frame_ret != AVERROR(EAGAIN) && frame_ret != AVERROR_EOF) {
            SetAvError("play avcodec_receive_frame: ", frame_ret);
            return -1;
        }

        av_packet_unref(play.pkt);
        const int read_ret = av_read_frame(play.fmt, play.pkt);
        if (read_ret < 0) {
            DrainDecodedAudio(play);

            ff::ClearLastError();
            return 1;
        }

        if (play.has_audio && play.pkt->stream_index == play.audio_stream) {
            if (avcodec_send_packet(play.audio_dec, play.pkt) < 0) {
                ff::SetLastError("play audio avcodec_send_packet failed");
                return -1;
            }
        } else if (play.pkt->stream_index == play.video_stream) {
            if (avcodec_send_packet(play.dec, play.pkt) < 0) {
                ff::SetLastError("play avcodec_send_packet failed");
                return -1;
            }
        }

        if (++loop_guard > 8192) {
            ff::SetLastError("play loop guard");
            return -1;
        }
    }
}

void FfPlayClose(FfSession& session)
{

    ReleasePlayState(session.play);
    ff::ClearLastError();
}

void FfPlayResetPace(FfSession& session)
{
    ResetPlayPace(session.play);
}

void FfPlayPause(FfSession& session)
{
    auto& play = session.play;
    if (!play.active || play.paused) {
        return;
    }
    play.paused = true;
    play.pause_started_wall_ms = GetTickCount64();
    if (play.wasapi_play) {
        FfPlayWasapiPause(static_cast<WasapiPlayback*>(play.wasapi_play));
    }

}

int FfPlaySetVolume(FfSession& session, int volume_percent)
{
    if (volume_percent < 0) {
        volume_percent = 0;
    } else if (volume_percent > 100) {
        volume_percent = 100;
    }

    session.play.volume_percent = volume_percent;
    if (!session.play.wasapi_play) {
        ff::ClearLastError();
        return 0;
    }

    if (!FfPlayWasapiSetVolume(static_cast<WasapiPlayback*>(session.play.wasapi_play), volume_percent)) {
        return -1;
    }

    ff::ClearLastError();
    return 0;
}

void FfPlayResume(FfSession& session)
{
    auto& play = session.play;
    if (!play.active || !play.paused) {
        return;
    }
    if (play.pause_started_wall_ms > 0 && play.pace_started) {
        const int64_t now = GetTickCount64();
        play.pace_base_wall_ms += (now - play.pause_started_wall_ms);
    }
    play.pause_started_wall_ms = 0;
    play.paused = false;
    if (play.wasapi_play) {
        FfPlayWasapiResume(static_cast<WasapiPlayback*>(play.wasapi_play));
    }

}

int FfPlaySeekRelativeMs(FfSession& session, int ms_delta)
{
    auto& play = session.play;
    if (!play.active || !play.fmt || play.video_stream < 0) {
        ff::SetLastError("play not active");
        return -1;
    }

    AVStream* stream = play.fmt->streams[play.video_stream];
    int64_t cur_ts = play.last_pts;
    if (cur_ts == AV_NOPTS_VALUE) {
        cur_ts = 0;
    }

    const AVRational ms_base{1, 1000};
    int64_t target = cur_ts + av_rescale_q(static_cast<int64_t>(ms_delta), ms_base, stream->time_base);
    if (target < 0) {
        target = 0;
    }

    if (av_seek_frame(play.fmt, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
        ff::SetLastError("av_seek_frame failed");
        return -1;
    }

    avcodec_flush_buffers(play.dec);
    if (play.audio_dec) {
        avcodec_flush_buffers(play.audio_dec);
    }
    if (play.wasapi_play) {
        FfPlayWasapiReset(static_cast<WasapiPlayback*>(play.wasapi_play));
    }
    play.last_pts = target;
    ResetPlayPace(play);
    ff::ClearLastError();
    return 0;
}
