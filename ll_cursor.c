/* ll_cursor.c — 四条边各自一根弹簧的光标 */
#include <string.h>
#include "ll_cursor.h"
#include "ll_anim.h"
#include "ll_geom.h"
#include "ll_theme.h"

static LlValue g_l, g_t, g_r, g_b;
static LlRect  g_base;          /* 收缩/探出之前的目标矩形 */
static int     g_have;          /* 第一次落点直接跳，不要从屏幕角上飞过来 */
static int     g_visible;
static int     g_pressed;
static int     g_lean_x, g_lean_y;

void ll_cursor_init(void)
{
    ll_value_init(&g_l, 0.0f, ll_spring_default);
    ll_value_init(&g_t, 0.0f, ll_spring_default);
    ll_value_init(&g_r, 0.0f, ll_spring_default);
    ll_value_init(&g_b, 0.0f, ll_spring_default);
    memset(&g_base, 0, sizeof(g_base));
    g_have = 0;
    g_visible = 0;
    g_pressed = 0;
    g_lean_x = g_lean_y = 0;
}

void ll_cursor_show(int on)  { g_visible = on ? 1 : 0; }
int  ll_cursor_visible(void) { return g_visible && g_have; }

static void all_springs(const LlSpring *s)
{
    ll_value_set_spring(&g_l, s);
    ll_value_set_spring(&g_t, s);
    ll_value_set_spring(&g_r, s);
    ll_value_set_spring(&g_b, s);
}

/* 把 base 应用"按住收缩"和"朝键探出"，写成四条边的目标。 */
static void apply(void)
{
    float inset = g_pressed ? 2.0f : 0.0f;
    float l = (float)g_base.x + inset;
    float t = (float)g_base.y + inset;
    float r = (float)ll_rect_right(g_base) - inset;
    float b = (float)ll_rect_bottom(g_base) - inset;

    if (g_lean_x < 0)      l += (float)g_lean_x;
    else if (g_lean_x > 0) r += (float)g_lean_x;
    if (g_lean_y < 0)      t += (float)g_lean_y;
    else if (g_lean_y > 0) b += (float)g_lean_y;

    ll_value_set(&g_l, l);
    ll_value_set(&g_t, t);
    ll_value_set(&g_r, r);
    ll_value_set(&g_b, b);
}

void ll_cursor_move(LlRect want)
{
    float dcx, dcy;
    int   long_jump;

    if (ll_rect_empty(want)) {
        g_visible = 0;
        return;
    }
    g_visible = 1;
    if (!g_have) {
        g_base = want;
        g_have = 1;
        ll_value_jump(&g_l, (float)want.x);
        ll_value_jump(&g_t, (float)want.y);
        ll_value_jump(&g_r, (float)ll_rect_right(want));
        ll_value_jump(&g_b, (float)ll_rect_bottom(want));
        return;
    }
    dcx = (want.x + want.w * 0.5f) - ((float)g_base.x + g_base.w * 0.5f);
    dcy = (want.y + want.h * 0.5f) - ((float)g_base.y + g_base.h * 0.5f);
    /* 跳得比自身尺寸的两倍还远，就当"跨页"处理 */
    long_jump = (ll_absf(dcy) > want.h * 2.0f) || (ll_absf(dcx) > want.w * 2.0f);
    g_base = want;

    if (long_jump) {
        all_springs(ll_spring_cursor_leap);   /* 落地带一点压扁 */
    } else if (ll_absf(dcy) >= ll_absf(dcx)) {
        /* 垂直走：行进那条边用 Lead，相反那条用 Trail -> 光标被拉长再收拢 */
        ll_value_set_spring(&g_b, dcy > 0 ? ll_spring_cursor_lead : ll_spring_cursor_trail);
        ll_value_set_spring(&g_t, dcy > 0 ? ll_spring_cursor_trail : ll_spring_cursor_lead);
        ll_value_set_spring(&g_l, ll_spring_cursor_lead);
        ll_value_set_spring(&g_r, ll_spring_cursor_lead);
    } else {
        ll_value_set_spring(&g_r, dcx > 0 ? ll_spring_cursor_lead : ll_spring_cursor_trail);
        ll_value_set_spring(&g_l, dcx > 0 ? ll_spring_cursor_trail : ll_spring_cursor_lead);
        ll_value_set_spring(&g_t, ll_spring_cursor_lead);
        ll_value_set_spring(&g_b, ll_spring_cursor_lead);
    }
    apply();
}

void ll_cursor_press(int down)
{
    if (g_pressed == (down ? 1 : 0)) {
        return;
    }
    g_pressed = down ? 1 : 0;
    /* 按住用 Release（带回弹，缩下去时有一点黏），松开用 Lead 干脆回到原样 */
    all_springs(g_pressed ? ll_spring_cursor_release : ll_spring_cursor_lead);
    apply();
}

void ll_cursor_lean(int dx, int dy)
{
    if (g_lean_x == dx && g_lean_y == dy) {
        return;
    }
    g_lean_x = dx;
    g_lean_y = dy;
    all_springs(ll_spring_cursor_lean);
    apply();
}

LlRect ll_cursor_rect(void)
{
    int x = ll_round_f(g_l.current);
    int y = ll_round_f(g_t.current);
    int r = ll_round_f(g_r.current);
    int b = ll_round_f(g_b.current);

    return (LlRect){ (int16_t)x, (int16_t)y, (int16_t)(r - x), (int16_t)(b - y) };
}

int ll_cursor_busy(void)
{
    return !ll_value_settled(&g_l) || !ll_value_settled(&g_t) ||
           !ll_value_settled(&g_r) || !ll_value_settled(&g_b);
}

static int16_t cursor_radius(LlRect r)
{
    int16_t rad = (int16_t)ll_round_f((float)ll_theme.radius * ll_unit());
    int16_t cap = (int16_t)(r.w / 2);

    if (r.h / 2 < cap) {
        cap = (int16_t)(r.h / 2);
    }
    return rad > cap ? cap : rad;
}

void ll_cursor_paint(LlCanvas c)
{
    LlRect r = ll_cursor_rect();

    if (!ll_cursor_visible() || ll_rect_empty(r) || !ll_canvas_visible(c, r)) {
        return;
    }
    if (ll_canvas_can_invert(c)) {
        ll_canvas_invert_round_rect(c, r, cursor_radius(r));
    } else {
        /* 不能反色的屏：描一圈边，至少看得见它在哪 */
        ll_canvas_stroke_round_rect(c, r, cursor_radius(r), ll_theme.accent);
    }
}
