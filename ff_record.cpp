#include "pch.h"
#include "ff_record_wasapi.h"
#include "ff_session.h"
#include "ff_util.h"

namespace {


void SetAvError(const char* prefix, int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    ff::SetLastError(std::string(prefix) + buf);
}

bool WriteRecordAudioPackets(FfSession::RecordState& rec);
bool SendRecordAudioFrame(FfSession::RecordState& rec);
bool PrepareRecordAudioFrame(FfSession::RecordState& rec, int nb_samples);
bool EncodeOneRecordAudioFrame(FfSession::RecordState& rec, const uint8_t* in_data, int in_samples);
void FlushRecordEncoderPackets(
    AVFormatContext* fmt,
    AVCodecContext* enc,
    AVStream* stream,
    AVPacket* pkt)
{
    if (!enc || !pkt || !fmt || !stream) {
        return;
    }

    avcodec_send_frame(enc, nullptr);
    for (;;) {
        av_packet_unref(pkt);
        const int ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }

        av_packet_rescale_ts(pkt, enc->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
    }
}

void FlushRecordAudioPending(FfSession::RecordState& rec)
{
    if (!rec.has_audio || rec.audio_pcm_pending.empty()) {
        return;
    }

    const int bytes_per_sample = rec.audio_channels * static_cast<int>(sizeof(int16_t));
    const int frame_bytes = rec.audio_frame_samples * bytes_per_sample;
    if (frame_bytes <= 0) {
        return;
    }

    while (static_cast<int>(rec.audio_pcm_pending.size()) >= frame_bytes) {
        const uint8_t* in_data = rec.audio_pcm_pending.data();
        if (!EncodeOneRecordAudioFrame(rec, in_data, rec.audio_frame_samples)) {
            break;
        }
        rec.audio_pcm_pending.erase(
            rec.audio_pcm_pending.begin(),
            rec.audio_pcm_pending.begin() + frame_bytes);
    }

    if (!rec.audio_pcm_pending.empty()) {
        rec.audio_pcm_pending.resize(static_cast<size_t>(frame_bytes), 0);
        EncodeOneRecordAudioFrame(rec, rec.audio_pcm_pending.data(), rec.audio_frame_samples);
        rec.audio_pcm_pending.clear();
    }
}

void ReleaseRecordState(FfSession::RecordState& rec)
{
    if (rec.has_audio) {
        FlushRecordAudioPending(rec);
    }

    if (rec.enc && rec.pkt) {
        FlushRecordEncoderPackets(rec.fmt, rec.enc, rec.stream, rec.pkt);
    }
    if (rec.has_audio && rec.audio_enc && rec.audio_pkt) {
        FlushRecordEncoderPackets(rec.fmt, rec.audio_enc, rec.audio_stream, rec.audio_pkt);
    }

    if (rec.fmt) {
        if (rec.active) {
            av_write_trailer(rec.fmt);
        }
        if (!(rec.fmt->oformat->flags & AVFMT_NOFILE) && rec.fmt->pb) {
            avio_closep(&rec.fmt->pb);
        }
        avformat_free_context(rec.fmt);
        rec.fmt = nullptr;
    }

    if (rec.swr) {
        swr_free(&rec.swr);
        rec.swr = nullptr;
    }

    FfReleaseSws(&rec.sws);
    FfReleaseFrame(&rec.frame_yuv);
    FfReleaseFrame(&rec.frame_bgra);
    FfReleaseFrame(&rec.audio_frame);
    FfReleasePacket(&rec.pkt);
    FfReleasePacket(&rec.audio_pkt);
    FfReleaseCodec(&rec.enc);
    FfReleaseCodec(&rec.audio_enc);

    rec.stream = nullptr;
    rec.audio_stream = nullptr;
    rec.next_pts = 0;
    rec.audio_pts = 0;
    rec.audio_pcm_pending.clear();
    rec.has_audio = false;
    rec.audio_sample_rate = 0;
    rec.audio_channels = 0;
    rec.audio_frame_samples = 0;
    rec.active = false;
}

AVSampleFormat FirstSupportedAudioSampleFmt(const AVCodec* codec, AVSampleFormat prefer)
{
    if (codec && codec->sample_fmts) {
        for (int i = 0; codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
            if (codec->sample_fmts[i] == prefer) {
                return prefer;
            }
        }
        return codec->sample_fmts[0];
    }
    if (codec && strcmp(codec->name, "aac_mf") == 0) {
        return AV_SAMPLE_FMT_S16;
    }
    return AV_SAMPLE_FMT_FLTP;
}

bool OpenRecordAudioEncoder(
    AVCodecContext* ctx,
    const AVCodec* codec,
    int sample_rate,
    int channels,
    AVSampleFormat sample_fmt,
    bool set_aac_profile,
    bool global_header)
{
    av_channel_layout_uninit(&ctx->ch_layout);
    ctx->sample_rate = sample_rate;
    ctx->time_base = AVRational{1, sample_rate};
    av_channel_layout_default(&ctx->ch_layout, channels);
    ctx->bit_rate = 128000;
    ctx->sample_fmt = sample_fmt;
    ctx->profile = FF_PROFILE_UNKNOWN;
    if (set_aac_profile) {
        ctx->profile = FF_PROFILE_AAC_LOW;
    }
    if (global_header) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = AVERROR_UNKNOWN;
    if (!FfSafeAvcodecOpen2(ctx, codec, &ret)) {
        ff::SetLastError("record audio avcodec_open2 crashed");
        return false;
    }
    if (ret < 0) {
        SetAvError("record audio avcodec_open2: ", ret);
        return false;
    }
    return true;
}

bool InitRecordAudio(FfSession::RecordState& rec, int sample_rate, int channels)
{
    if (sample_rate <= 0 || channels <= 0 || channels > 2) {
        ff::SetLastError("record audio sample_rate/channels invalid (use 1 or 2 channels)");
        return false;
    }

    rec.audio_stream = avformat_new_stream(rec.fmt, nullptr);
    if (!rec.audio_stream) {
        ff::SetLastError("avformat_new_stream audio failed");
        return false;
    }

    const bool global_header = (rec.fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;

    const AVCodec* codec_candidates[4] = {};
    int candidate_count = 0;

    auto try_add = [&](const AVCodec* c) {
        if (!c || candidate_count >= 4) {
            return;
        }
        for (int i = 0; i < candidate_count; ++i) {
            if (codec_candidates[i] == c) {
                return;
            }
        }
        codec_candidates[candidate_count++] = c;
    };

    try_add(avcodec_find_encoder_by_name("aac_mf"));
    try_add(avcodec_find_encoder(AV_CODEC_ID_AAC));
    try_add(avcodec_find_encoder_by_name("aac"));

    for (int i = 0; i < candidate_count; ++i) {
        const AVCodec* codec = codec_candidates[i];
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            continue;
        }

        ctx->codec_type = AVMEDIA_TYPE_AUDIO;
        ctx->codec_id = codec->id;

        const AVSampleFormat prefer = (strcmp(codec->name, "aac_mf") == 0)
            ? AV_SAMPLE_FMT_S16
            : AV_SAMPLE_FMT_FLTP;
        const AVSampleFormat sample_fmt = FirstSupportedAudioSampleFmt(codec, prefer);
        const bool set_profile = strcmp(codec->name, "aac") == 0;

        if (OpenRecordAudioEncoder(
                ctx,
                codec,
                sample_rate,
                channels,
                sample_fmt,
                set_profile,
                global_header)) {
            rec.audio_enc = ctx;

            break;
        }

        avcodec_free_context(&ctx);
    }

    if (!rec.audio_enc) {
        ff::SetLastError("record audio avcodec_open2 failed (aac_mf/aac)");

        return false;
    }

    rec.audio_stream->time_base = rec.audio_enc->time_base;

    if (avcodec_parameters_from_context(rec.audio_stream->codecpar, rec.audio_enc) < 0) {
        ff::SetLastError("record audio avcodec_parameters_from_context failed");
        return false;
    }

    const unsigned int aac_tag =
        av_codec_get_tag(rec.fmt->oformat->codec_tag, AV_CODEC_ID_AAC);
    if (aac_tag != 0) {
        rec.audio_stream->codecpar->codec_tag = aac_tag;
    }

    rec.audio_pkt = av_packet_alloc();
    rec.audio_frame = av_frame_alloc();
    if (!rec.audio_pkt || !rec.audio_frame) {
        ff::SetLastError("record audio packet/frame alloc failed");
        return false;
    }

    AVChannelLayout in_layout{};
    av_channel_layout_default(&in_layout, channels);
    const int swr_ret = swr_alloc_set_opts2(
        &rec.swr,
        &rec.audio_enc->ch_layout,
        rec.audio_enc->sample_fmt,
        rec.audio_enc->sample_rate,
        &in_layout,
        AV_SAMPLE_FMT_S16,
        sample_rate,
        0,
        nullptr);
    av_channel_layout_uninit(&in_layout);
    if (swr_ret < 0 || !rec.swr || swr_init(rec.swr) < 0) {
        ff::SetLastError("record audio swr_init failed");
        return false;
    }

    rec.audio_frame->format = rec.audio_enc->sample_fmt;
    rec.audio_frame->sample_rate = rec.audio_enc->sample_rate;
    av_channel_layout_copy(&rec.audio_frame->ch_layout, &rec.audio_enc->ch_layout);

    rec.audio_frame_samples = rec.audio_enc->frame_size > 0 ? rec.audio_enc->frame_size : 1024;
    rec.audio_sample_rate = sample_rate;
    rec.audio_channels = channels;
    rec.audio_pts = 0;
    rec.audio_pcm_pending.clear();
    rec.has_audio = true;
    ff::ClearLastError();
    return true;
}

bool WriteRecordVideoPackets(FfSession::RecordState& rec)
{
    while (true) {
        av_packet_unref(rec.pkt);
        const int ret = avcodec_receive_packet(rec.enc, rec.pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            SetAvError("record avcodec_receive_packet: ", ret);
            return false;
        }

        av_packet_rescale_ts(rec.pkt, rec.enc->time_base, rec.stream->time_base);
        rec.pkt->stream_index = rec.stream->index;
        if (av_interleaved_write_frame(rec.fmt, rec.pkt) < 0) {
            ff::SetLastError("av_interleaved_write_frame video failed");
            av_packet_unref(rec.pkt);
            return false;
        }
        av_packet_unref(rec.pkt);
    }
    return true;
}

bool WriteRecordAudioPackets(FfSession::RecordState& rec)
{
    while (true) {
        av_packet_unref(rec.audio_pkt);
        const int ret = avcodec_receive_packet(rec.audio_enc, rec.audio_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            SetAvError("record audio avcodec_receive_packet: ", ret);
            return false;
        }

        av_packet_rescale_ts(rec.audio_pkt, rec.audio_enc->time_base, rec.audio_stream->time_base);
        rec.audio_pkt->stream_index = rec.audio_stream->index;
        if (av_interleaved_write_frame(rec.fmt, rec.audio_pkt) < 0) {
            ff::SetLastError("av_interleaved_write_frame audio failed");
            av_packet_unref(rec.audio_pkt);
            return false;
        }
        av_packet_unref(rec.audio_pkt);
    }
    return true;
}

bool PrepareRecordAudioFrame(FfSession::RecordState& rec, int nb_samples)
{
    if (!rec.audio_enc || !rec.audio_frame || nb_samples <= 0) {
        return false;
    }

    const bool need_buffer =
        !rec.audio_frame->data[0]
        || rec.audio_frame->nb_samples != nb_samples
        || rec.audio_frame->format != rec.audio_enc->sample_fmt
        || rec.audio_frame->sample_rate != rec.audio_enc->sample_rate;

    if (need_buffer) {
        av_frame_unref(rec.audio_frame);
        rec.audio_frame->format = rec.audio_enc->sample_fmt;
        rec.audio_frame->sample_rate = rec.audio_enc->sample_rate;
        if (av_channel_layout_copy(&rec.audio_frame->ch_layout, &rec.audio_enc->ch_layout) < 0) {
            ff::SetLastError("record audio channel layout copy failed");
            return false;
        }
        rec.audio_frame->nb_samples = nb_samples;
        if (av_frame_get_buffer(rec.audio_frame, 0) < 0) {
            ff::SetLastError("record audio av_frame_get_buffer failed");
            return false;
        }
        return true;
    }

    rec.audio_frame->nb_samples = nb_samples;
    if (av_frame_make_writable(rec.audio_frame) < 0) {
        ff::SetLastError("record audio frame writable failed");
        return false;
    }
    return true;
}

bool SendRecordAudioFrame(FfSession::RecordState& rec)
{
    if (avcodec_send_frame(rec.audio_enc, rec.audio_frame) < 0) {
        ff::SetLastError("record audio avcodec_send_frame failed");
        return false;
    }
    return WriteRecordAudioPackets(rec);
}

bool EncodeOneRecordAudioFrame(FfSession::RecordState& rec, const uint8_t* in_data, int in_samples)
{
    if (!rec.audio_enc || !rec.audio_frame || !in_data || in_samples <= 0) {
        return false;
    }

    if (!PrepareRecordAudioFrame(rec, in_samples)) {
        return false;
    }

    const int bytes_per_sample = rec.audio_channels * static_cast<int>(sizeof(int16_t));
    const int in_bytes = in_samples * bytes_per_sample;

    if (rec.audio_enc->sample_fmt == AV_SAMPLE_FMT_S16) {
        if (!rec.audio_frame->data[0]) {
            ff::SetLastError("record audio S16 frame buffer missing");
            return false;
        }
        memcpy(rec.audio_frame->data[0], in_data, static_cast<size_t>(in_bytes));
    } else {
        if (!rec.swr) {
            ff::SetLastError("record audio resampler missing");
            return false;
        }

        const int converted = swr_convert(
            rec.swr,
            rec.audio_frame->data,
            in_samples,
            &in_data,
            in_samples);
        if (converted < 0) {
            ff::SetLastError("record audio swr_convert failed");
            return false;
        }
        if (converted != in_samples) {
            ff::SetLastError("record audio swr_convert short output");
            return false;
        }
    }

    rec.audio_frame->nb_samples = in_samples;
    rec.audio_frame->pts = rec.audio_pts;
    rec.audio_pts += in_samples;
    return SendRecordAudioFrame(rec);
}

bool FeedRecordAudioPcm16(FfSession::RecordState& rec, const uint8_t* pcm, int byte_len)
{
    if (!rec.has_audio || !pcm || byte_len <= 0) {
        ff::SetLastError("record audio not enabled or invalid pcm");
        return false;
    }

    const int bytes_per_sample = rec.audio_channels * static_cast<int>(sizeof(int16_t));
    if (byte_len % bytes_per_sample != 0) {
        ff::SetLastError("record audio pcm byte length not aligned to samples");
        return false;
    }

    rec.audio_pcm_pending.insert(rec.audio_pcm_pending.end(), pcm, pcm + byte_len);

    const int frame_bytes = rec.audio_frame_samples * bytes_per_sample;
    while (static_cast<int>(rec.audio_pcm_pending.size()) >= frame_bytes) {
        const uint8_t* in_data = rec.audio_pcm_pending.data();
        if (!EncodeOneRecordAudioFrame(rec, in_data, rec.audio_frame_samples)) {
            return false;
        }

        rec.audio_pcm_pending.erase(
            rec.audio_pcm_pending.begin(),
            rec.audio_pcm_pending.begin() + frame_bytes);
    }

    ff::ClearLastError();
    return true;
}

bool FeedRecordBgra(FfSession& session, FfSession::RecordState& rec, const uint8_t* bgra, int capture_bytes)
{
    const int expected = session.src_width * session.src_height * 4;
    if (capture_bytes > 0 && capture_bytes < expected) {
        ff::SetLastError("BGRA capture length too small for record");
        return false;
    }

    if (!rec.frame_bgra) {
        rec.frame_bgra = av_frame_alloc();
        rec.frame_yuv = av_frame_alloc();
        rec.pkt = av_packet_alloc();
        if (!rec.frame_bgra || !rec.frame_yuv || !rec.pkt) {
            ff::SetLastError("record frame alloc failed");
            return false;
        }
        rec.frame_bgra->format = AV_PIX_FMT_BGRA;
        rec.frame_bgra->width = session.src_width;
        rec.frame_bgra->height = session.src_height;
        rec.frame_yuv->format = rec.enc->pix_fmt;
        rec.frame_yuv->width = session.enc_width;
        rec.frame_yuv->height = session.enc_height;
        if (av_frame_get_buffer(rec.frame_bgra, 32) < 0 || av_frame_get_buffer(rec.frame_yuv, 32) < 0) {
            ff::SetLastError("record frame buffer failed");
            return false;
        }
    }

    if (av_frame_make_writable(rec.frame_bgra) < 0 || av_frame_make_writable(rec.frame_yuv) < 0) {
        ff::SetLastError("record frame writable failed");
        return false;
    }

    const int line = session.src_width * 4;
    for (int y = 0; y < session.src_height; ++y) {
        memcpy(
            rec.frame_bgra->data[0] + static_cast<size_t>(y) * rec.frame_bgra->linesize[0],
            bgra + static_cast<size_t>(y) * line,
            static_cast<size_t>(line));
    }

    if (!rec.sws) {
        rec.sws = sws_getContext(
            session.src_width,
            session.src_height,
            AV_PIX_FMT_BGRA,
            session.enc_width,
            session.enc_height,
            rec.enc->pix_fmt,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!rec.sws) {
            ff::SetLastError("record sws_getContext failed");
            return false;
        }
    }

    sws_scale(
        rec.sws,
        rec.frame_bgra->data,
        rec.frame_bgra->linesize,
        0,
        session.src_height,
        rec.frame_yuv->data,
        rec.frame_yuv->linesize);

    rec.frame_yuv->pts = rec.next_pts++;
    if (avcodec_send_frame(rec.enc, rec.frame_yuv) < 0) {
        ff::SetLastError("record avcodec_send_frame failed");
        return false;
    }

    if (!WriteRecordVideoPackets(rec)) {
        return false;
    }

    ff::ClearLastError();
    return true;
}

bool BeginRecordMp4(FfSession& session, const char* path_gbk, int sample_rate, int channels)
{
    if (!path_gbk || !path_gbk[0]) {
        ff::SetLastError("record path empty");
        return false;
    }

    auto& rec = session.record;
    ReleaseRecordState(rec);

    const std::string path_utf8 = ff::GbkToUtf8(path_gbk);

    if (avformat_alloc_output_context2(&rec.fmt, nullptr, "mp4", path_utf8.c_str()) < 0 || !rec.fmt) {
        ff::SetLastError("avformat_alloc_output_context2 failed");
        return false;
    }

    const AVCodec* vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec) {
        ff::SetLastError("H264 encoder not found for record");
        ReleaseRecordState(rec);
        return false;
    }

    rec.stream = avformat_new_stream(rec.fmt, nullptr);
    if (!rec.stream) {
        ff::SetLastError("avformat_new_stream failed");
        ReleaseRecordState(rec);
        return false;
    }

    rec.enc = avcodec_alloc_context3(vcodec);
    if (!rec.enc) {
        ff::SetLastError("record avcodec_alloc_context3 failed");
        ReleaseRecordState(rec);
        return false;
    }

    const int fps = session.encoder_fps > 0 ? session.encoder_fps : 30;
    rec.enc->width = session.enc_width;
    rec.enc->height = session.enc_height;
    rec.enc->time_base = AVRational{1, fps};
    rec.enc->framerate = AVRational{fps, 1};
    rec.enc->gop_size = fps;
    rec.enc->max_b_frames = 0;
    rec.enc->pix_fmt = AV_PIX_FMT_YUV420P;
    rec.enc->bit_rate = ff::BitrateFromKbPerSec(session.bitrate_kb);
    if (rec.enc->priv_data) {
        av_opt_set(rec.enc->priv_data, "preset", "ultrafast", 0);
        av_opt_set(rec.enc->priv_data, "tune", "zerolatency", 0);
    }

    if (rec.fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        rec.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int open_ret = AVERROR_UNKNOWN;
    if (!FfSafeAvcodecOpen2(rec.enc, vcodec, &open_ret) || open_ret < 0) {
        if (open_ret < 0) {
            SetAvError("record avcodec_open2: ", open_ret);
        } else {
            ff::SetLastError("record avcodec_open2 crashed");
        }
        ReleaseRecordState(rec);
        return false;
    }

    if (avcodec_parameters_from_context(rec.stream->codecpar, rec.enc) < 0) {
        ff::SetLastError("avcodec_parameters_from_context failed");
        ReleaseRecordState(rec);
        return false;
    }

    if (session.record.wasapi_capture) {
        int wasapi_rate = 0;
        int wasapi_ch = 0;
        if (!FfRecordWasapiGetEncodeFormat(session, wasapi_rate, wasapi_ch)) {
            ff::SetLastError("record audio WASAPI format invalid");
            ReleaseRecordState(rec);
            return false;
        }
        if (!InitRecordAudio(rec, wasapi_rate, wasapi_ch)) {
            ReleaseRecordState(rec);
            return false;
        }
    } else if (sample_rate > 0 && channels > 0) {
        if (!InitRecordAudio(rec, sample_rate, channels)) {
            ReleaseRecordState(rec);
            return false;
        }
    }

    if (!(rec.fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&rec.fmt->pb, path_utf8.c_str(), AVIO_FLAG_WRITE) < 0) {
            ff::SetLastError("avio_open failed");
            ReleaseRecordState(rec);
            return false;
        }
    }

    if (avformat_write_header(rec.fmt, nullptr) < 0) {
        ff::SetLastError("avformat_write_header failed");
        ReleaseRecordState(rec);
        return false;
    }

    rec.next_pts = 0;
    rec.active = true;

    ff::ClearLastError();
    return true;
}

} // namespace

int FfRecordBegin(FfSession& session, const char* path_gbk, int record_audio)
{
    FfRecordEnd(session);

    if (record_audio == 0) {
        const int ret = BeginRecordMp4(session, path_gbk, 0, 0) ? 0 : -1;

        return ret;
    }

    int sample_rate = 0;
    int channels = 0;
    if (!FfRecordWasapiOpen(session, sample_rate, channels)) {
        ff::SetLastError("record audio WASAPI open failed");
        return -1;
    }


    if (channels > 2) {
        channels = 2;
    }

    if (!BeginRecordMp4(session, path_gbk, sample_rate, channels)) {
        FfRecordWasapiClose(session);
        return -1;
    }

    if (!session.record.has_audio) {
        ff::SetLastError("record audio stream not created");

        FfRecordEnd(session);
        return -1;
    }

    return 0;
}

void FfRecordFrameBgra(FfSession& session, const uint8_t* bgra, int capture_bytes)
{
    if (!session.record.active || !bgra) {
        ff::SetLastError("record not active or invalid frame");

        return;
    }
    if (!FeedRecordBgra(session, session.record, bgra, capture_bytes)) {
        return;
    }
    if (session.record.has_audio) {
        if (!FfRecordWasapiCaptureForVideoFrame(session)) {
            return;
        }
    }
}

bool FfRecordFeedAudioPcm16(FfSession& session, const uint8_t* pcm, int byte_len)
{
    if (!session.record.active || !session.record.has_audio) {
        ff::SetLastError("record audio not enabled");
        return false;
    }
    if (!FeedRecordAudioPcm16(session.record, pcm, byte_len)) {
        return false;
    }
    return true;
}

bool FfRecordHasAudio(const FfSession& session)
{
    return session.record.active && session.record.has_audio;
}

void FfRecordEnd(FfSession& session)
{

    FfRecordWasapiFlushRemaining(session);
    FfRecordWasapiClose(session);
    ReleaseRecordState(session.record);

}
