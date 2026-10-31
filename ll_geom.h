/* ll_geom.h — 布局空间的几何类型（移植自 lilt 的 ui/geom.go）
 *
 * 布局一律用 float32 算，这样动画位置是平滑的；只有真正下笔时才取整到像素。
 * Padding / Margin / Gap / 圆角都以"单位"给出：一个单位 = 主题字体行高 / 默认字体行高，
 * 默认字体下正好 1 像素；换大字体时所有间距一起放大。
 */
#ifndef LL_GEOM_H
#define LL_GEOM_H

#include <stdint.h>
#include "ll_gfx.h"

typedef struct { float x, y; } LlVec2;

/* 布局矩形（浮点）。像素版是 LlRect。 */
typedef struct { float x, y, w, h; } LlRectF;

/* 内/外边距，四边分别给。 */
typedef struct { float left, top, right, bottom; } LlInsets;

#define LL_UNBOUNDED (1.0f * (1 << 24))   /* 滚动轴上的"无限可用空间" */

float   ll_rectf_right(LlRectF r);
float   ll_rectf_bottom(LlRectF r);
LlInsets ll_insets_all(float v);
LlInsets ll_insets_xy(float x, float y);
LlInsets ll_insets_ltrb(float l, float t, float r, float b);
float    ll_insets_horizontal(LlInsets i);
float    ll_insets_vertical(LlInsets i);
LlInsets ll_insets_scale(LlInsets i, float f);

/* 把相邻的两条边各自取整再相减，这样取整后仍然严丝合缝（lilt 的 Pixels()）。 */
LlRect ll_rectf_pixels(LlRectF r);

/* 四舍五入，.5 远离 0（对应 ui.Round）。 */
int16_t ll_round_f(float x);

float ll_clampf(float x, float lo, float hi);
float ll_absf(float x);

#endif /* LL_GEOM_H */
