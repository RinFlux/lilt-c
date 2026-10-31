/* ll_geom.c — 几何辅助 */
#include "ll_geom.h"

float ll_rectf_right(LlRectF r)  { return r.x + r.w; }
float ll_rectf_bottom(LlRectF r) { return r.y + r.h; }

LlInsets ll_insets_all(float v) { return (LlInsets){ v, v, v, v }; }

LlInsets ll_insets_xy(float x, float y) { return (LlInsets){ x, y, x, y }; }

LlInsets ll_insets_ltrb(float l, float t, float r, float b)
{
    LlInsets i;
    i.left = l; i.top = t; i.right = r; i.bottom = b;
    return i;
}

float ll_insets_horizontal(LlInsets i) { return i.left + i.right; }
float ll_insets_vertical(LlInsets i)   { return i.top + i.bottom; }

LlInsets ll_insets_scale(LlInsets i, float f)
{
    i.left *= f; i.top *= f; i.right *= f; i.bottom *= f;
    return i;
}

int16_t ll_round_f(float x)
{
    if (x < 0.0f) {
        return (int16_t)(-(-x + 0.5f));
    }
    return (int16_t)(x + 0.5f);
}

LlRect ll_rectf_pixels(LlRectF r)
{
    int16_t x = ll_round_f(r.x);
    int16_t y = ll_round_f(r.y);
    LlRect  out;

    out.x = x;
    out.y = y;
    out.w = (int16_t)(ll_round_f(r.x + r.w) - x);
    out.h = (int16_t)(ll_round_f(r.y + r.h) - y);
    return out;
}

float ll_clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

float ll_absf(float x) { return x < 0.0f ? -x : x; }
