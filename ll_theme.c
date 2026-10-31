/* ll_theme.c — 主题默认值与光标物理参数 */
#include "ll_theme.h"

LlTheme ll_theme = {
    /* background */ {   0,   0,   0, 255 },
    /* foreground */ { 255, 255, 255, 255 },
    /* accent     */ { 255, 255, 255, 255 },
    /* muted      */ { 128, 128, 128, 255 },
    LL_FONT_BODY,
    LL_FONT_SMALL,
    3,                          /* radius，单位 */
    { 4, 1, 4, 1 },             /* RowPadding    = XY(4,1) */
    { 6, 2, 6, 2 },             /* ButtonPadding = XY(6,2) */
    2.0f,                       /* Gap */
    { 2, 2, 2, 2 },             /* PanelPadding  = All(2) */
};

/* refLine：默认字体（notosans 12pt）的行高，此时一个单位就是一像素。 */
#define LL_REF_LINE 15.0f

float ll_unit(void)
{
    return (float)ll_font_line_height(ll_theme.font) / LL_REF_LINE;
}

static LlSpring g_cursor_lead    = { 0.22f, 1.0f, 1 };  /* Ease：一帧就全力出发，不回弹 */
static LlSpring g_cursor_trail   = { 0.52f, 1.0f, 1 };
static LlSpring g_cursor_release = { 0.57f, 0.40f, 0 }; /* 带回弹，松手时抖一下 */
static LlSpring g_cursor_leap    = { 0.63f, 0.45f, 0 }; /* 跨屏跳过去，落地压扁 */
static LlSpring g_cursor_lean    = { 0.35f, 0.70f, 0 }; /* 朝按住的键先探一点 */

const LlSpring *const ll_spring_cursor_lead    = &g_cursor_lead;
const LlSpring *const ll_spring_cursor_trail   = &g_cursor_trail;
const LlSpring *const ll_spring_cursor_release = &g_cursor_release;
const LlSpring *const ll_spring_cursor_leap    = &g_cursor_leap;
const LlSpring *const ll_spring_cursor_lean    = &g_cursor_lean;
