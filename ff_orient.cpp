#include "pch.h"
#include "ff_orient.h"
#include "ff_session.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdint>

namespace {

inline int PixelLuma(const uint8_t* p, int bytes_per_pixel)
{
    if (bytes_per_pixel == 1) {
        return p[0];
    }
    return (static_cast<int>(p[0]) + static_cast<int>(p[1]) * 2 + static_cast<int>(p[2])) >> 2;
}

constexpr int kContentLuma = 28;
constexpr int kBlackLuma = 14;

int SampleRowBrightRatio(
    const uint8_t* pixels,
    int width,
    int pitch,
    int bytes_per_pixel,
    int y,
    int x0,
    int x1,
    int step)
{
    const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(pitch);
    int bright = 0;
    int total = 0;
    for (int x = x0; x < x1; x += step) {
        const uint8_t* px = pixels + row_off + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel);
        if (PixelLuma(px, bytes_per_pixel) > kContentLuma) {
            ++bright;
        }
        ++total;
    }
    if (total <= 0) {
        return 0;
    }
    return bright * 100 / total;
}

int BandDarkPercent(
    const uint8_t* pixels,
    int width,
    int height,
    int pitch,
    int bytes_per_pixel,
    int capacity,
    int y0,
    int y1,
    int x0,
    int x1,
    int step)
{
    if (y1 <= y0) {
        return 100;
    }

    int dark = 0;
    int total = 0;
    const int row_bytes = width * bytes_per_pixel;
    const int max_y = pitch > 0 ? (std::min)(height, capacity / pitch) : height;

    for (int y = y0; y < y1 && y < max_y; y += step) {
        const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(pitch);
        if (row_off + static_cast<size_t>(row_bytes) > static_cast<size_t>(capacity)) {
            break;
        }
        for (int x = x0; x < x1; x += step) {
            const uint8_t* px = pixels + row_off + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel);
            if (PixelLuma(px, bytes_per_pixel) <= kBlackLuma) {
                ++dark;
            }
            ++total;
        }
    }

    if (total <= 0) {
        return 100;
    }
    return dark * 100 / total;
}

bool DetectContentRectInBand(
    const uint8_t* pixels,
    int width,
    int height,
    int pitch,
    int bytes_per_pixel,
    int capacity,
    int band_y0,
    int band_y1,
    int* out_x0,
    int* out_y0,
    int* out_x1,
    int* out_y1)
{
    const int max_y = pitch > 0 ? (std::min)(height, capacity / pitch) : height;
    const int step = (std::max)(width, max_y) > 720 ? 8 : 4;
    const int y_begin = (std::max)(0, band_y0);
    const int y_end = (std::min)(max_y, band_y1);

    int x0 = width;
    int x1 = 0;
    int y0 = y_end;
    int y1 = y_begin;

    for (int y = y_begin; y < y_end; y += step) {
        const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(pitch);
        if (row_off + static_cast<size_t>(width) * static_cast<size_t>(bytes_per_pixel)
            > static_cast<size_t>(capacity)) {
            break;
        }

        const int row_bright_pct = SampleRowBrightRatio(pixels, width, pitch, bytes_per_pixel, y, 0, width, step);
        if (row_bright_pct < 2) {
            continue;
        }

        for (int x = 0; x < width; x += step) {
            const uint8_t* px = pixels + row_off + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel);
            if (PixelLuma(px, bytes_per_pixel) > kContentLuma) {
                x0 = (std::min)(x0, x);
                x1 = (std::max)(x1, x);
            }
        }
        y0 = (std::min)(y0, y);
        y1 = (std::max)(y1, y);
    }

    if (x1 <= x0 || y1 < y0) {
        return false;
    }

    *out_x0 = x0;
    *out_y0 = y0;
    *out_x1 = (std::min)(width, x1 + step);
    *out_y1 = (std::min)(height, y1 + step);
    return true;
}

bool DetectLetterboxBandY(
    const uint8_t* pixels,
    int width,
    int height,
    int pitch,
    int bytes_per_pixel,
    int capacity,
    int* out_y0,
    int* out_y1)
{
    if (width >= height) {
        return false;
    }

    const int step = width > 720 ? 6 : 4;
    const int bottom_y0 = height * 45 / 100;
    const int bottom_dark = BandDarkPercent(
        pixels, width, height, pitch, bytes_per_pixel, capacity, bottom_y0, height, 0, width, step);
    if (bottom_dark < 72) {
        return false;
    }

    int content_y0 = height;
    int content_y1 = 0;
    const int scan_y1 = height * 70 / 100;
    for (int y = 0; y < scan_y1; y += step) {
        if (SampleRowBrightRatio(pixels, width, pitch, bytes_per_pixel, y, 0, width, step) >= 3) {
            content_y0 = (std::min)(content_y0, y);
            content_y1 = (std::max)(content_y1, y);
        }
    }

    if (content_y1 <= content_y0) {
        return false;
    }

    *out_y0 = content_y0;
    *out_y1 = (std::min)(height, content_y1 + step);
    return true;
}

void AlignContentRect(int width, int height, int* x0, int* y0, int* x1, int* y1)
{
    *x0 = (std::max)(0, (*x0 / 2) * 2);
    *y0 = (std::max)(0, (*y0 / 2) * 2);
    *x1 = (std::min)(width, ((*x1 + 1) / 2) * 2);
    *y1 = (std::min)(height, ((*y1 + 1) / 2) * 2);
    if (*x1 <= *x0 + 16) {
        *x0 = 0;
        *x1 = width;
    }
    if (*y1 <= *y0 + 16) {
        *y0 = 0;
        *y1 = height;
    }
}

void UpdateContentRect(
    FfSession& session,
    const uint8_t* pixels,
    int width,
    int height,
    int pitch,
    int bytes_per_pixel,
    int capacity)
{
    session.content_rect_valid = false;
    if (!pixels || width < 32 || height < 32 || pitch <= 0 || capacity < width * bytes_per_pixel) {
        return;
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = width;
    int y1 = height;

    if (width > height) {
        session.content_x0 = 0;
        session.content_y0 = 0;
        session.content_x1 = width;
        session.content_y1 = height;
        session.content_rect_valid = true;
        return;
    }

    int band_y0 = 0;
    int band_y1 = height;
    const bool letterbox_band = DetectLetterboxBandY(
        pixels, width, height, pitch, bytes_per_pixel, capacity, &band_y0, &band_y1);

    if (!DetectContentRectInBand(
            pixels, width, height, pitch, bytes_per_pixel, capacity, band_y0, band_y1, &x0, &y0, &x1, &y1)) {
        if (letterbox_band) {
            x0 = 0;
            y0 = band_y0;
            x1 = width;
            y1 = band_y1;
        } else {
            session.content_x0 = 0;
            session.content_y0 = 0;
            session.content_x1 = width;
            session.content_y1 = height;
            session.content_rect_valid = true;
            return;
        }
    }

    AlignContentRect(width, height, &x0, &y0, &x1, &y1);

    const int crop_w = x1 - x0;
    const int crop_h = y1 - y0;
    if (crop_w < 32 || crop_h < 32 || crop_w * crop_h < width * height / 5) {
        session.content_x0 = 0;
        session.content_y0 = 0;
        session.content_x1 = width;
        session.content_y1 = height;
    } else {
        session.content_x0 = x0;
        session.content_y0 = y0;
        session.content_x1 = x1;
        session.content_y1 = y1;
    }
    session.content_rect_valid = true;
}

void ApplyOrient(FfSession& session, int frame_w, int frame_h)
{
    if (frame_w > frame_h) {
        session.orient_landscape = 1;
        session.orient_valid = true;
        return;
    }

    if (session.content_rect_valid) {
        const int crop_w = session.content_x1 - session.content_x0;
        const int crop_h = session.content_y1 - session.content_y0;
        if (crop_w > 16 && crop_h > 16) {
            session.orient_landscape = crop_w > crop_h ? 1 : 0;
            session.orient_valid = true;
            return;
        }
    }

    session.orient_landscape = 0;
    session.orient_valid = true;
}

} // namespace

void FfOrientReset(FfSession& session)
{
    session.orient_valid = false;
    session.orient_landscape = 0;
    session.dec_frame_dims_valid = false;
    session.dec_frame_w = 0;
    session.dec_frame_h = 0;
    session.content_rect_valid = false;
    session.content_x0 = 0;
    session.content_y0 = 0;
    session.content_x1 = 0;
    session.content_y1 = 0;
    session.dec_sws_src_w = 0;
    session.dec_sws_src_h = 0;
    session.dec_sws_dst_w = 0;
    session.dec_sws_dst_h = 0;
}

void FfOrientUpdateFromAvFrame(FfSession& session, AVFrame* frame)
{
    if (!frame || frame->width < 32 || frame->height < 32 || !frame->data[0]) {
        session.orient_valid = false;
        session.content_rect_valid = false;
        return;
    }

    const int pitch = frame->linesize[0] > 0 ? frame->linesize[0] : frame->width;
    UpdateContentRect(
        session,
        frame->data[0],
        frame->width,
        frame->height,
        pitch,
        1,
        pitch * frame->height);
    ApplyOrient(session, frame->width, frame->height);
}

void FfOrientUpdateFromBgra(
    FfSession& session,
    const uint8_t* bgra,
    int width,
    int height,
    int pitch,
    int capacity)
{
    if (session.orient_valid) {
        return;
    }

    if (!bgra || width < 32 || height < 32 || pitch < width * 4 || capacity < width * 4) {
        session.orient_valid = false;
        return;
    }

    UpdateContentRect(session, bgra, width, height, pitch, 4, capacity);
    ApplyOrient(session, width, height);
}

int FfOrientIsLandscape(const FfSession& session)
{
    if (session.orient_valid) {
        return session.orient_landscape;
    }
    return session.src_width > session.src_height ? 1 : 0;
}
