/* ll_gfx.c — Canvas 与 Rect 的实现（对应 lilt 的 gfx/canvas.go、gfx/rect.go、gfx/text.go） */
#include <stddef.h>
#include "ll_gfx.h"

const LlColor ll_black  = {   0,   0,   0, 255 };
const LlColor ll_white  = { 255, 255, 255, 255 };
const LlColor ll_gray128= { 128, 128, 128, 255 };

/* 单色屏判亮：按 3:6:1 加权的亮度不低于一半。主题的 Muted 灰因此有确定结果。 */
int ll_is_lit(LlColor c)
{
    return (int)c.r * 3 + (int)c.g * 6 + (int)c.b >= 128 * 10;
}

/* ---------------- Rect ---------------- */

int16_t ll_rect_right(LlRect r)  { return (int16_t)(r.x + r.w); }
int16_t ll_rect_bottom(LlRect r) { return (int16_t)(r.y + r.h); }
int     ll_rect_empty(LlRect r)  { return r.w <= 0 || r.h <= 0; }

int ll_rect_contains(LlRect r, int16_t x, int16_t y)
{
    return x >= r.x && y >= r.y && x < ll_rect_right(r) && y < ll_rect_bottom(r);
}

LlRect ll_rect_offset(LlRect r, int16_t dx, int16_t dy)
{
    r.x = (int16_t)(r.x + dx);
    r.y = (int16_t)(r.y + dy);
    return r;
}

LlRect ll_rect_inset(LlRect r, int16_t d)
{
    r.x = (int16_t)(r.x + d);
    r.y = (int16_t)(r.y + d);
    r.w = (int16_t)(r.w - 2 * d);
    r.h = (int16_t)(r.h - 2 * d);
    return r;
}

LlRect ll_rect_intersect(LlRect r, LlRect o)
{
    int16_t x0 = r.x > o.x ? r.x : o.x;
    int16_t y0 = r.y > o.y ? r.y : o.y;
    int16_t x1 = ll_rect_right(r) < ll_rect_right(o) ? ll_rect_right(r) : ll_rect_right(o);
    int16_t y1 = ll_rect_bottom(r) < ll_rect_bottom(o) ? ll_rect_bottom(r) : ll_rect_bottom(o);
    LlRect  out = { 0, 0, 0, 0 };

    if (x1 <= x0 || y1 <= y0) {
        return out;
    }
    out.x = x0;
    out.y = y0;
    out.w = (int16_t)(x1 - x0);
    out.h = (int16_t)(y1 - y0);
    return out;
}

int ll_rect_overlaps(LlRect r, LlRect o) { return !ll_rect_empty(ll_rect_intersect(r, o)); }

/* ---------------- Canvas ---------------- */

LlCanvas ll_canvas_new(LlScreen *s)
{
    LlCanvas c;

    c.s   = s;
    c.clip.x = 0;
    c.clip.y = 0;
    c.clip.w = s->w;
    c.clip.h = s->h;
    c.ox  = 0;
    c.oy  = 0;
    return c;
}

LlCanvas ll_canvas_translate(LlCanvas c, int16_t dx, int16_t dy)
{
    c.ox = (int16_t)(c.ox + dx);
    c.oy = (int16_t)(c.oy + dy);
    return c;
}

LlCanvas ll_canvas_clip(LlCanvas c, LlRect r)
{
    c.clip = ll_rect_intersect(c.clip, ll_rect_offset(r, c.ox, c.oy));
    return c;
}

LlCanvas ll_canvas_absolute(LlCanvas c)
{
    c.ox = 0;
    c.oy = 0;
    return c;
}

LlRect ll_canvas_bounds(LlCanvas c) { return ll_rect_offset(c.clip, (int16_t)-c.ox, (int16_t)-c.oy); }

int ll_canvas_visible(LlCanvas c, LlRect r)
{
    return ll_rect_overlaps(c.clip, ll_rect_offset(r, c.ox, c.oy));
}

int ll_canvas_can_invert(LlCanvas c) { return c.s && c.s->invert_rect != NULL; }

void ll_canvas_origin(LlCanvas c, int16_t *x, int16_t *y)
{
    if (x) *x = c.ox;
    if (y) *y = c.oy;
}

void ll_canvas_pixel(LlCanvas c, int16_t x, int16_t y, LlColor col)
{
    x = (int16_t)(x + c.ox);
    y = (int16_t)(y + c.oy);
    if (ll_rect_contains(c.clip, x, y)) {
        c.s->set_pixel(c.s, x, y, col);
    }
}

void ll_canvas_fill_rect(LlCanvas c, LlRect r, LlColor col)
{
    r = ll_rect_intersect(c.clip, ll_rect_offset(r, c.ox, c.oy));
    if (!ll_rect_empty(r) && c.s->fill_rect) {
        c.s->fill_rect(c.s, r.x, r.y, r.w, r.h, col);
    }
}

void ll_canvas_invert_rect(LlCanvas c, LlRect r)
{
    if (!c.s->invert_rect) {
        return;
    }
    r = ll_rect_intersect(c.clip, ll_rect_offset(r, c.ox, c.oy));
    if (!ll_rect_empty(r)) {
        c.s->invert_rect(c.s, r.x, r.y, r.w, r.h);
    }
}

void ll_canvas_hline(LlCanvas c, int16_t x, int16_t y, int16_t w, LlColor col)
{
    LlRect r = { x, y, w, 1 };
    ll_canvas_fill_rect(c, r, col);
}

void ll_canvas_vline(LlCanvas c, int16_t x, int16_t y, int16_t h, LlColor col)
{
    LlRect r = { x, y, 1, h };
    ll_canvas_fill_rect(c, r, col);
}

void ll_canvas_stroke_rect(LlCanvas c, LlRect r, LlColor col)
{
    if (ll_rect_empty(r)) {
        return;
    }
    ll_canvas_hline(c, r.x, r.y, r.w, col);
    ll_canvas_hline(c, r.x, (int16_t)(ll_rect_bottom(r) - 1), r.w, col);
    ll_canvas_vline(c, r.x, (int16_t)(r.y + 1), (int16_t)(r.h - 2), col);
    ll_canvas_vline(c, (int16_t)(ll_rect_right(r) - 1), (int16_t)(r.y + 1), (int16_t)(r.h - 2), col);
}

/* 牛顿迭代整数开方（lilt 用它画圆角，避免浮点 sqrt）。 */
static int32_t isqrt_i32(int32_t n)
{
    int32_t x, y;

    if (n <= 0) {
        return 0;
    }
    x = n;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

/* 半径为 r 的圆角，第 i 行要往里缩多少。+r 是"四舍五入到最近像素"，
 * 小半径才不会看起来是方的。 */
static int16_t corner_inset(int16_t r, int16_t i)
{
    int32_t dy = r - i;
    int32_t rr = r;
    int32_t dx = isqrt_i32(rr * rr - dy * dy + rr);

    return (int16_t)(r - dx);
}

static void canvas_span(LlCanvas c, LlRect r, LlColor col, int invert)
{
    if (invert) {
        ll_canvas_invert_rect(c, r);
    } else {
        ll_canvas_fill_rect(c, r, col);
    }
}

/* 圆角按行扫描：每页一次调用，填充和反色的代价都是"每行一次"。
 * lilt 用一个 flag 而不是回调参数，是为了每帧绘制不分配闭包——C 里天然如此。 */
static void round_spans(LlCanvas c, LlRect r, int16_t radius, LlColor col, int invert)
{
    LlRect body;
    int16_t i;

    if (ll_rect_empty(r)) {
        return;
    }
    if (radius > r.w / 2) radius = (int16_t)(r.w / 2);
    if (radius > r.h / 2) radius = (int16_t)(r.h / 2);
    if (radius <= 0) {
        canvas_span(c, r, col, invert);
        return;
    }
    body.x = r.x;
    body.y = (int16_t)(r.y + radius);
    body.w = r.w;
    body.h = (int16_t)(r.h - 2 * radius);
    if (!ll_rect_empty(body)) {
        canvas_span(c, body, col, invert);
    }
    for (i = 0; i < radius; i++) {
        int16_t in = corner_inset(radius, i);
        LlRect  top = { (int16_t)(r.x + in), (int16_t)(r.y + i), (int16_t)(r.w - 2 * in), 1 };
        LlRect  bot = { (int16_t)(r.x + in), (int16_t)(ll_rect_bottom(r) - 1 - i), (int16_t)(r.w - 2 * in), 1 };
        canvas_span(c, top, col, invert);
        canvas_span(c, bot, col, invert);
    }
}

void ll_canvas_fill_round_rect(LlCanvas c, LlRect r, int16_t radius, LlColor col)
{
    round_spans(c, r, radius, col, 0);
}

void ll_canvas_invert_round_rect(LlCanvas c, LlRect r, int16_t radius)
{
    round_spans(c, r, radius, ll_black, 1);
}

void ll_canvas_stroke_round_rect(LlCanvas c, LlRect r, int16_t radius, LlColor col)
{
    int16_t i, in, w, prev = radius;

    if (ll_rect_empty(r)) {
        return;
    }
    if (radius > r.w / 2) radius = (int16_t)(r.w / 2);
    if (radius > r.h / 2) radius = (int16_t)(r.h / 2);
    if (radius <= 0) {
        ll_canvas_stroke_rect(c, r, col);
        return;
    }
    ll_canvas_hline(c, (int16_t)(r.x + radius), r.y, (int16_t)(r.w - 2 * radius), col);
    ll_canvas_hline(c, (int16_t)(r.x + radius), (int16_t)(ll_rect_bottom(r) - 1), (int16_t)(r.w - 2 * radius), col);
    ll_canvas_vline(c, r.x, (int16_t)(r.y + radius), (int16_t)(r.h - 2 * radius), col);
    ll_canvas_vline(c, (int16_t)(ll_rect_right(r) - 1), (int16_t)(r.y + radius), (int16_t)(r.h - 2 * radius), col);
    for (i = 0; i < radius; i++) {
        int16_t yt = (int16_t)(r.y + i);
        int16_t yb = (int16_t)(ll_rect_bottom(r) - 1 - i);

        in = corner_inset(radius, i);
        /* 和上一行的缩进取差，保证圆弧陡峭处不断线 */
        w = (int16_t)(prev - in);
        if (w < 1) w = 1;
        if (i == 0) w = (int16_t)(radius - in);
        ll_canvas_hline(c, (int16_t)(r.x + in), yt, w, col);
        ll_canvas_hline(c, (int16_t)(ll_rect_right(r) - in - w), yt, w, col);
        ll_canvas_hline(c, (int16_t)(r.x + in), yb, w, col);
        ll_canvas_hline(c, (int16_t)(ll_rect_right(r) - in - w), yb, w, col);
        prev = in;
    }
}

void ll_canvas_fill_circle(LlCanvas c, int16_t cx, int16_t cy, int16_t r, LlColor col)
{
    LlRect box = { (int16_t)(cx - r), (int16_t)(cy - r), (int16_t)(2 * r + 1), (int16_t)(2 * r + 1) };
    ll_canvas_fill_round_rect(c, box, (int16_t)(r + 1), col);
}

void ll_canvas_stroke_circle(LlCanvas c, int16_t cx, int16_t cy, int16_t r, LlColor col)
{
    LlRect box = { (int16_t)(cx - r), (int16_t)(cy - r), (int16_t)(2 * r + 1), (int16_t)(2 * r + 1) };
    ll_canvas_stroke_round_rect(c, box, (int16_t)(r + 1), col);
}

/* Bresenham 直线，够用画勾和叉这类图标。 */
void ll_canvas_line(LlCanvas c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, LlColor col)
{
    int16_t dx = (int16_t)(x1 - x0), dy = (int16_t)(y1 - y0);
    int16_t sx = 1, sy = 1, err, e2;

    if (dx < 0) { dx = (int16_t)-dx; sx = -1; }
    if (dy < 0) { dy = (int16_t)-dy; sy = -1; }
    err = (int16_t)(dx - dy);
    for (;;) {
        ll_canvas_pixel(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        e2 = (int16_t)(2 * err);
        if (e2 > -dy) { err = (int16_t)(err - dy); x0 = (int16_t)(x0 + sx); }
        if (e2 < dx)  { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
    }
}

/* 位图字形是"每行 w 位、MSB 优先、不补齐到字节"的连续位流。 */
int16_t ll_canvas_text(LlCanvas c, const LlFont *f, int16_t x, int16_t y, const char *s, LlColor col)
{
    const char *p = s;

    if (!f || !s) {
        return x;
    }
    while (*p) {
        uint32_t          rune = ll_font_utf8_next(&p);
        const tf_glyph_t  *g   = tf_font_find(f, rune);
        tf_glyph_t         fb;
        int16_t            gx, gy;
        unsigned int       i, j;

        if (g == NULL) {
            tf_font_missing_glyph(f, &fb);
            g = &fb;                 /* 缺字形不画墨，但笔锋照常前进 */
        }
        gx = (int16_t)(x + g->x_offset);
        gy = (int16_t)(y + g->y_offset);
        if (ll_canvas_visible(c, (LlRect){ gx, gy, g->width, g->height })) {
            for (j = 0; j < g->height; j++) {
                for (i = 0; i < g->width; i++) {
                    if (tf_glyph_pixel(f, g, i, j)) {
                        ll_canvas_pixel(c, (int16_t)(gx + i), (int16_t)(gy + j), col);
                    }
                }
            }
        }
        x = (int16_t)(x + g->x_advance);
    }
    return x;
}
