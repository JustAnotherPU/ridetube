/* Where VPP puts the picture: the frame size, the picture's rect in it and
 * the source rect it reads, by display aspect (sample aspect ratio included,
 * 0 = square). The decoder's start() uses it; player_test checks it (host).
 *
 * Fit: the picture inside out_w x out_h, black bars around; filled instead
 * when that crops at most 15 % (a 2.35:1 film on the 8:3 screen, not a 16:9
 * one). fill: always cropped to fill it. out_w 0: the source's own size.
 * no_upscale: the frame is the picture alone (no bars), at most the source's
 * resolution: the renderer scales it on the GPU into the same place. */
#ifndef YTDEC_FIT_H
#define YTDEC_FIT_H

#include <stdint.h>

namespace woc {

struct Fit {
    uint32_t w, h;                          /* frame */
    uint16_t pic_x, pic_y, pic_w, pic_h;    /* the picture in the frame (VPP Out crop) */
    uint16_t src_x, src_y, src_w, src_h;    /* the source rect VPP reads (VPP In crop) */
};

inline uint16_t fit_even(double v) { return (uint16_t)((uint32_t)(v + 0.5) & ~1u); }

inline Fit fit_frame(uint16_t cx, uint16_t cy, uint16_t cw, uint16_t ch, uint16_t sar_w, uint16_t sar_h,
                     uint32_t out_w, uint32_t out_h, bool fill, bool no_upscale)
{
    Fit f;
    f.w = out_w ? out_w : cw;
    f.h = out_h ? out_h : ch;
    f.pic_x = f.pic_y = 0, f.pic_w = (uint16_t)f.w, f.pic_h = (uint16_t)f.h;
    f.src_x = cx, f.src_y = cy, f.src_w = cw, f.src_h = ch;
    if (!out_w || !cw || !ch)
        return f;
    const double sar = sar_w && sar_h ? (double)sar_w / sar_h : 1.0;
    const double src = (double)cw * sar / ch, dst = (double)f.w / f.h;
    if (fill || (src > dst ? dst / src : src / dst) >= 0.85) {
        if (src > dst) {
            f.src_w = fit_even(cw * dst / src);
            f.src_x = (uint16_t)(cx + ((cw - f.src_w) / 2 & ~1u));
        } else {
            f.src_h = fit_even(ch * src / dst);
            f.src_y = (uint16_t)(cy + ((ch - f.src_h) / 2 & ~1u));
        }
    } else {
        if (src > dst)
            f.pic_h = fit_even(f.w / src);
        else
            f.pic_w = fit_even(f.h * src);
        if (f.pic_w < 2) f.pic_w = 2;
        if (f.pic_h < 2) f.pic_h = 2;
        f.pic_x = (uint16_t)((f.w - f.pic_w) / 2 & ~1u);
        f.pic_y = (uint16_t)((f.h - f.pic_h) / 2 & ~1u);
    }
    if (no_upscale) {
        /* larger than the source (by its rows): the source's rows, the picture's aspect (nearest even width, so
         * the renderer's fit puts it where the bars did, within a pixel) */
        if (f.pic_h > f.src_h) {
            const uint16_t h = (uint16_t)(f.src_h & ~1u);
            f.pic_w = (uint16_t)((uint32_t)((double)h * f.pic_w / f.pic_h / 2 + 0.5) * 2);
            f.pic_h = h;
        }
        if (f.pic_w < 16) f.pic_w = 16;   /* MGVF's smallest frame */
        if (f.pic_h < 16) f.pic_h = 16;
        f.w = f.pic_w, f.h = f.pic_h, f.pic_x = f.pic_y = 0;
    }
    return f;
}

}  // namespace woc

#endif
