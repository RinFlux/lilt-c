/* ll_screen.c — 页式帧缓冲（对应 lilt 的 driver/sh1106/{draw,sh1106}.go） */
#include "ll_screen.h"
#include "ll_bus.h"

LlMonoScreen ll_mono;

static int16_t clamp_i16(int16_t v, int16_t lo, int16_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* 列区间 x..x+w-1 裁到 0..limit-1，返回 [x0, x1)。 */
static void clamp_cols(int16_t x, int16_t w, int16_t limit, int16_t *x0, int16_t *x1)
{
    *x0 = clamp_i16(x, 0, limit);
    *x1 = clamp_i16(x + w, 0, limit);
}

static void clamp_rows(int16_t y, int16_t h, int16_t limit, int16_t *y0, int16_t *y1)
{
    *y0 = clamp_i16(y, 0, limit);
    *y1 = clamp_i16(y + h, 0, limit);
}

/* 这一页里被行区间覆盖到的位掩码：lo/hi 是该页内相对第一行的上下界。 */
static uint8_t page_mask(int16_t y0, int16_t y1, int16_t page)
{
    int16_t lo = y0 - (page << 3);
    int16_t hi = y1 - (page << 3);
    if (lo < 0) lo = 0;
    if (hi > 8) hi = 8;
    if (hi <= lo) return 0;
    return (uint8_t)((uint8_t)(0xFFu << lo) & (uint8_t)(0xFFu >> (8u - hi)));
}

void ll_screen_mark_dirty(int16_t y, int16_t h)
{
    int16_t y0, y1, pg;

    if (h <= 0) {
        return;
    }
    clamp_rows(y, h, LL_SCREEN_H, &y0, &y1);
    if (y0 >= y1) {
        return;
    }
    for (pg = (int16_t)(y0 >> 3); pg <= (int16_t)((y1 - 1) >> 3); pg++) {
        ll_mono.dirty |= (uint8_t)(1u << pg);
    }
}

static void mono_set_pixel(LlScreen *s, int16_t x, int16_t y, LlColor c)
{
    uint8_t bit;

    (void)s;
    /* 转成无符号一次比较就同时管住两端的越界（负数变成大数）。 */
    if ((uint16_t)x >= (uint16_t)LL_SCREEN_W || (uint16_t)y >= (uint16_t)LL_SCREEN_H) {
        return;
    }
    bit = (uint8_t)(1u << (y & 7));
    if (ll_is_lit(c)) {
        ll_mono.buf[(y >> 3) * LL_SCREEN_W + x] |= bit;
    } else {
        ll_mono.buf[(y >> 3) * LL_SCREEN_W + x] &= (uint8_t)~bit;
    }
    ll_mono.dirty |= (uint8_t)(1u << (y >> 3));
}

static void mono_fill_rect(LlScreen *s, int16_t x, int16_t y, int16_t w, int16_t h, LlColor c)
{
    int16_t x0, x1, y0, y1, pg;
    int     lit = ll_is_lit(c);

    (void)s;
    clamp_cols(x, w, LL_SCREEN_W, &x0, &x1);
    clamp_rows(y, h, LL_SCREEN_H, &y0, &y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (pg = (int16_t)(y0 >> 3); pg <= (int16_t)((y1 - 1) >> 3); pg++) {
        uint8_t  mask = page_mask(y0, y1, pg);
        uint8_t *row  = &ll_mono.buf[pg * LL_SCREEN_W];

        if (mask == 0xFFu) {
            /* 整页覆盖：直接写，省掉读-改-写。lilt 注释里特意点了这个优化。 */
            for (int16_t i = x0; i < x1; i++) {
                row[i] = lit ? 0xFFu : 0x00u;
            }
        } else if (lit) {
            for (int16_t i = x0; i < x1; i++) {
                row[i] |= mask;
            }
        } else {
            for (int16_t i = x0; i < x1; i++) {
                row[i] &= (uint8_t)~mask;
            }
        }
        ll_mono.dirty |= (uint8_t)(1u << pg);
    }
}

static void mono_invert_rect(LlScreen *s, int16_t x, int16_t y, int16_t w, int16_t h)
{
    int16_t x0, x1, y0, y1, pg;

    (void)s;
    clamp_cols(x, w, LL_SCREEN_W, &x0, &x1);
    clamp_rows(y, h, LL_SCREEN_H, &y0, &y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (pg = (int16_t)(y0 >> 3); pg <= (int16_t)((y1 - 1) >> 3); pg++) {
        uint8_t mask = page_mask(y0, y1, pg);
        for (int16_t i = x0; i < x1; i++) {
            ll_mono.buf[pg * LL_SCREEN_W + i] ^= mask;
        }
        ll_mono.dirty |= (uint8_t)(1u << pg);
    }
}

static int mono_flush(LlScreen *s)
{
    (void)s;
    return ll_screen_flush();
}

void ll_screen_clear(LlColor c)
{
    uint8_t v = ll_is_lit(c) ? 0xFFu : 0x00u;

    for (int i = 0; i < LL_SCREEN_W * LL_SCREEN_PAGES; i++) {
        ll_mono.buf[i] = v;
    }
    ll_mono.dirty = (uint8_t)((1u << LL_SCREEN_PAGES) - 1u);
}

int ll_screen_flush(void)
{
    if (ll_mono.dirty == 0) {
        return 0;          /* 没变化就一次总线都不碰 */
    }
    for (int16_t pg = 0; pg < LL_SCREEN_PAGES; pg++) {
        if ((ll_mono.dirty & (uint8_t)(1u << pg)) == 0) {
            continue;
        }
        ll_bus_push_page(pg, &ll_mono.buf[pg * LL_SCREEN_W], LL_SCREEN_W);
        ll_mono.dirty &= (uint8_t)~(1u << pg);
    }
    return ll_bus_flush();
}

uint8_t ll_screen_dirty_mask(void) { return ll_mono.dirty; }

void ll_screen_init(void)
{
    ll_mono.api.w           = LL_SCREEN_W;
    ll_mono.api.h           = LL_SCREEN_H;
    ll_mono.api.set_pixel   = mono_set_pixel;
    ll_mono.api.fill_rect   = mono_fill_rect;
    ll_mono.api.invert_rect = mono_invert_rect;   /* 能反色 => 光标用 XOR，和 lilt 的 SH1106 一样 */
    ll_mono.api.flush       = mono_flush;
    ll_mono.api.impl        = &ll_mono;
    ll_mono.dirty           = 0;
    ll_screen_clear(ll_black);
    ll_screen_mark_dirty(0, 0);
    ll_bus_begin();
}

LlScreen *ll_screen_api(void) { return &ll_mono.api; }
