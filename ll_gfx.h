/* ll_gfx.h — 绘制层：Screen 接口 + 带裁剪与平移的 Canvas（移植自 lilt 的 gfx 包）
 *
 * 两层结构刻意保持和 Go 版一样：
 *   LlScreen 是"一块屏"，硬件相关（SSD1306 帧缓冲 / 主机 PPM 都实现它）；
 *   LlCanvas 是"这块屏上一个平移过、裁剪过的视野"，纯软件，按值传递。
 * Translate/Clip 返回新的 Canvas 而不改原来的，所以嵌套绘制不需要保存/恢复状态。
 */
#ifndef LL_GFX_H
#define LL_GFX_H

#include <stdint.h>
#include "ll_font.h"

/* 颜色用 RGBA，单色屏靠 ll_is_lit() 决定亮不亮——这样主题里的 Muted 灰
 * 在单色屏上也有确定的结果（和 lilt 完全一样的 3:6:1 加权）。 */
typedef struct { uint8_t r, g, b, a; } LlColor;

extern const LlColor ll_black;
extern const LlColor ll_white;
extern const LlColor ll_gray128;      /* 主题的 Muted */

int ll_is_lit(LlColor c);              /* 单色屏：这个颜色该点亮吗 */

/* 像素矩形（对应 gfx.Rect）。 */
typedef struct { int16_t x, y, w, h; } LlRect;

int16_t ll_rect_right(LlRect r);
int16_t ll_rect_bottom(LlRect r);
int     ll_rect_empty(LlRect r);
int     ll_rect_contains(LlRect r, int16_t x, int16_t y);
LlRect  ll_rect_offset(LlRect r, int16_t dx, int16_t dy);
LlRect  ll_rect_inset(LlRect r, int16_t d);
LlRect  ll_rect_intersect(LlRect r, LlRect o);
int     ll_rect_overlaps(LlRect r, LlRect o);

/* 一块屏。invert_rect 可以为 NULL，表示这块屏不能反色（光标就改画底色块）。 */
typedef struct LlScreen {
    int16_t w, h;
    void (*set_pixel)(struct LlScreen *s, int16_t x, int16_t y, LlColor c);
    void (*fill_rect)(struct LlScreen *s, int16_t x, int16_t y, int16_t w, int16_t h, LlColor c);
    void (*invert_rect)(struct LlScreen *s, int16_t x, int16_t y, int16_t w, int16_t h);
    int  (*flush)(struct LlScreen *s);
    void *impl;
} LlScreen;

typedef struct {
    LlScreen *s;
    LlRect    clip;   /* 屏幕坐标 */
    int16_t   ox, oy; /* 本地原点在屏幕上的位置 */
} LlCanvas;

LlCanvas ll_canvas_new(LlScreen *s);
LlCanvas ll_canvas_translate(LlCanvas c, int16_t dx, int16_t dy);
LlCanvas ll_canvas_clip(LlCanvas c, LlRect r);      /* r 用本地坐标 */
LlCanvas ll_canvas_absolute(LlCanvas c);            /* 原点回到屏幕角，保留裁剪 */
LlRect   ll_canvas_bounds(LlCanvas c);              /* 裁剪区回到本地坐标 */
int      ll_canvas_visible(LlCanvas c, LlRect r);
int      ll_canvas_can_invert(LlCanvas c);
void     ll_canvas_origin(LlCanvas c, int16_t *x, int16_t *y);

void    ll_canvas_pixel(LlCanvas c, int16_t x, int16_t y, LlColor col);
void    ll_canvas_fill_rect(LlCanvas c, LlRect r, LlColor col);
void    ll_canvas_invert_rect(LlCanvas c, LlRect r);
void    ll_canvas_hline(LlCanvas c, int16_t x, int16_t y, int16_t w, LlColor col);
void    ll_canvas_vline(LlCanvas c, int16_t x, int16_t y, int16_t h, LlColor col);
void    ll_canvas_stroke_rect(LlCanvas c, LlRect r, LlColor col);
void    ll_canvas_fill_round_rect(LlCanvas c, LlRect r, int16_t radius, LlColor col);
void    ll_canvas_invert_round_rect(LlCanvas c, LlRect r, int16_t radius);
void    ll_canvas_stroke_round_rect(LlCanvas c, LlRect r, int16_t radius, LlColor col);
void    ll_canvas_fill_circle(LlCanvas c, int16_t cx, int16_t cy, int16_t r, LlColor col);
void    ll_canvas_stroke_circle(LlCanvas c, int16_t cx, int16_t cy, int16_t r, LlColor col);
void    ll_canvas_line(LlCanvas c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, LlColor col);

/* 画一串文字，基线在 y，返回最后一个字形之后的 x。UTF-8，缺字形跳过但照常进笔。 */
int16_t ll_canvas_text(LlCanvas c, const LlFont *f, int16_t x, int16_t y, const char *s, LlColor col);

#endif /* LL_GFX_H */
