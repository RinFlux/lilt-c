/* ll_theme.h — 主题与全局物理参数（移植自 lilt 的 ui/theme.go）
 *
 * 颜色、字体、圆角、各类内边距都在这一份结构里；组件构造时读它，
 * 所以要改就在建树之前改。lilt 是"运行时改一处全局生效"的风格，这里保持一致。
 */
#ifndef LL_THEME_H
#define LL_THEME_H

#include "ll_anim.h"
#include "ll_font.h"
#include "ll_geom.h"
#include "ll_gfx.h"

typedef struct {
    LlColor      background;
    LlColor      foreground;
    LlColor      accent;      /* 不能反色的屏上，光标画成这个颜色的底色块 */
    LlColor      muted;       /* 提示文字、次要文字 */
    const LlFont *font;       /* 正文 */
    const LlFont *small_font; /* 小字 */
    int16_t      radius;      /* 圆角，单位 */
    LlInsets     row_padding;    /* 列表行、输入框、标题的文字内边距 */
    LlInsets     button_padding; /* 按钮文字到边框 */
    float        gap;              /* 并排控件之间 */
    LlInsets     panel_padding;    /* 弹窗、带边框面板里，控件离边框的距离 */
} LlTheme;

/* 单例主题，默认浅字深底，适合 OLED。 */
extern LlTheme ll_theme;

/* 一个单位等于多少像素：主题字体行高 / 参考行高(15)。 */
float ll_unit(void);

/* ---- 光标运动参数（lilt 里和主题放在同一份文件） ----
 * 行进方向的边用 Lead 缓动、相反方向的边用更慢的 Trail，
 * 于是光标朝目标"拉长"，到位时再"收拢"。 */
extern const LlSpring *const ll_spring_cursor_lead;    /* 0.22 纯缓动 */
extern const LlSpring *const ll_spring_cursor_trail;   /* 0.52 纯缓动 */
extern const LlSpring *const ll_spring_cursor_release; /* 0.57 / 0.4 松手带回弹 */
extern const LlSpring *const ll_spring_cursor_leap;    /* 0.63 / 0.45 跨屏落地带压扁 */
extern const LlSpring *const ll_spring_cursor_lean;    /* 0.35 / 0.7 朝按住的键探出去 */

#endif /* LL_THEME_H */
