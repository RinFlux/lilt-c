/* ll_font.h — lilt 视角的字体接口（数据来自 tf_*.c，由 tinyfont 转换而来）
 *
 * lilt 的 gfx/text.go 只做三件事：二分找字形、按基线定位、把 MSB 优先的连续位流
 * 逐点画出来。tf_font.h 里的 tf_glyph_pixel()/tf_font_find() 已经是同一套语义，
 * 这一层再补上 lilt 需要的 ascent/line-height 计算和 UTF-8 解码。
 */
#ifndef LL_FONT_H
#define LL_FONT_H

#include "tf_font.h"
#include "tf_fonts.h"

typedef tf_font_t LlFont;
typedef tf_glyph_t LlGlyph;

/* 主题正文与小字，名字对应 lilt 里的 notosans.Notosans12pt / proggy.TinySZ8pt7b。 */
#define LL_FONT_BODY   (&tf_notosans12pt)
#define LL_FONT_SMALL  (&tf_tinysz8pt7b)

/* Ascent = -BBox[3]：最高字形顶部到基线的距离（对应 lilt 的 gfx.Ascent）。 */
int16_t ll_font_ascent(const LlFont *f);

/* LineHeight = max(YAdvance, BBox[1])：一行字占的高度（对应 gfx.LineHeight）。
 * lilt 用它换算"单位"，一个单位 = 主题字体行高 / 默认字体行高。 */
int16_t ll_font_line_height(const LlFont *f);

/* 整串文字的推进宽度（UTF-8）。 */
int16_t ll_font_text_width(const LlFont *f, const char *s);

/* 从 s 读出一个码点并前进指针；*s 会被推进。遇到 '\0' 返回 0。 */
uint32_t ll_font_utf8_next(const char **s);

#endif /* LL_FONT_H */
