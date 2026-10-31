/* demo_spring.c — A 阶段验收页实现
 *
 * 三个区域分别对应 lilt 手感里最容易被认出来的三件事：
 *   1 光标条：朝新位置拉长、到位收拢（Lead 快、Trail 慢的纯缓动）
 *   2 波浪行：整列跳一格，每一行各用一根"离焦点越远越慢"的弹簧滑回来；
 *     焦点行反色，这就是最终固件里光标的雏形
 *   3 橡皮筋 + 冲量：按到底还继续按，整列按 UIScrollView 的阻尼被推下去，
 *     松手弹回；MENU 给所有值一记 Bump，静止的东西也会弹一下再回去
 *
 * 单色屏的教训（第一版就是踩在这里）：面板只有亮/灭两态，
 * ll_is_lit() 是硬阈值（和 lilt 的 IsLit 一模一样：3R+6G+B >= 1280），
 * 所以 96 灰、64 灰根本不会亮，"深浅"只能靠形状表达——
 * 描边 vs 实心、正文 vs 反色块。这里全页只用主题的三种画法。
 */
#include <stdio.h>
#include "demo_spring.h"
#include "ll_anim.h"
#include "ll_app.h"
#include "ll_bus.h"
#include "ll_gfx.h"
#include "ll_input.h"
#include "ll_theme.h"

/* ---- 版面尺寸（128x64，小字行高 10、墨迹 7） ---- */
#define HEAD_BASE    8          /* 页头基线 */
#define HEAD_LINE    10         /* 页头下划线 */

#define BAR_X0       8
#define BAR_X1       126
#define BAR_Y        12
#define BAR_H        7
#define BAR_SLOTS    4
#define SLOT_W       ((BAR_X1 - BAR_X0) / BAR_SLOTS)   /* 29 */

#define LIST_X       4
#define LIST_W       118
#define ROW_H        9          /* 一行的推进高度（含 2 像素缝） */
#define LIST_TOP     21
#define ROW_VIEW     4          /* 视口里几行 */
#define ROW_TOTAL    12         /* 一共几行 */
#define ROW_ASCENT   8          /* 行顶到基线 */

#define STATUS_BASE  64         /* 底行基线，墨迹占 57..63 */
#define PULL_DIM     18.0f      /* 橡皮筋的"最多还能推这么远" */

/* ---- 动画状态 ---- */
static LlValue v_edge_a, v_edge_b;   /* 光标条左右边 */
static int     slot;                 /* 当前槽位 0..3 */

static LlValue v_row[ROW_TOTAL];     /* 每行自己的位移 */
static LlSpring wave_springs[8];     /* 与 lilt 的 waveSprings 同源 */
static int     focus_row;            /* 焦点行 0..ROW_TOTAL-1 */
static int     scroll_row;           /* 视口起点行 */

static LlValue v_pull;               /* 超滚的原始位移 */
static float   pull_raw;             /* 按得越久推得越深 */

static char    status[24] = "";

/* 光标条的目标左右边界：槽位 i 占 [BAR_X0 + i*SLOT_W + 2, 宽 SLOT_W-4] */
static void bar_targets(int which, float *x0, float *x1)
{
    *x0 = (float)(BAR_X0 + which * SLOT_W + 2);
    *x1 = *x0 + (SLOT_W - 4);
}

static void move_bar(int to)
{
    float x0, x1, a, b;

    bar_targets(to, &x0, &x1);
    a = ll_value_get(&v_edge_a);
    b = ll_value_get(&v_edge_b);

    /* 行进方向的那条边用 Lead（快），相反方向用 Trail（慢）——
     * 于是条先被拉长，再收拢到目标。这就是 lilt 光标"有弹性"的全部秘密。 */
    if (x0 > a || x1 > b) {
        v_edge_a.spring = ll_spring_cursor_trail;
        v_edge_b.spring = ll_spring_cursor_lead;
    } else {
        v_edge_a.spring = ll_spring_cursor_lead;
        v_edge_b.spring = ll_spring_cursor_trail;
    }
    ll_value_set(&v_edge_a, x0);
    ll_value_set(&v_edge_b, x1);
    slot = to;
}

/* 视口跟着焦点走：焦点上下各留 (ROW_VIEW/2 - 1) 行 */
static int wanted_scroll(int f)
{
    int s = f - (ROW_VIEW / 2);

    if (s < 0) {
        s = 0;
    }
    if (s > ROW_TOTAL - ROW_VIEW) {
        s = ROW_TOTAL - ROW_VIEW;
    }
    return s;
}

/* 整列跳一格：内容 Jump 到新位置，每行 Shift 回原处再用各自的弹簧滑过去。
 * 这正是 lilt 的 Scroll.wave()。 */
static void step_wave(int new_focus)
{
    int   ns, d, k, i;
    float delta_px;

    if (new_focus < 0) {
        new_focus = 0;
    }
    if (new_focus >= ROW_TOTAL) {
        new_focus = ROW_TOTAL - 1;
    }
    ns = wanted_scroll(new_focus);
    d  = ns - scroll_row;
    focus_row  = new_focus;
    scroll_row = ns;
    if (d == 0) {
        return;               /* 视口没动，就不用给十二根弹簧重新排程 */
    }
    delta_px = (float)(-d * ROW_H);   /* 内容动了多少像素 */

    for (i = 0; i < ROW_TOTAL; i++) {
        k = i - focus_row;
        if (k < 0) {
            k = -k;
        }
        if (k > 7) {
            k = 7;
        }
        /* Response = Default.Response * (1 + stagger*k)，越远越慢；stagger=0.2 */
        wave_springs[k].response = ll_spring_default->response * (1.0f + 0.2f * (float)k);
        wave_springs[k].damping  = ll_spring_default->damping;
        wave_springs[k].ease     = 0;
        ll_value_set_spring(&v_row[i], &wave_springs[k]);
        ll_value_shift(&v_row[i], -delta_px);   /* 先留在屏幕上原位，然后滑过去 */
    }
}

/* 超滚：raw 是"想推多远"，画出来的是橡皮筋映射后的实际位移。 */
static void pull_to(float raw, const char *why)
{
    pull_raw = raw;
    ll_value_set(&v_pull, raw);
    if (why) {
        snprintf(status, sizeof(status), "%s", why);
    }
}

void ll_demo_spring_reset(void)
{
    float x0, x1;

    ll_anim_init();
    slot       = 0;
    focus_row  = 0;
    scroll_row = 0;
    pull_raw   = 0.0f;
    status[0]  = '\0';

    bar_targets(0, &x0, &x1);
    ll_value_init(&v_edge_a, x0, ll_spring_cursor_trail);
    ll_value_init(&v_edge_b, x1, ll_spring_cursor_lead);
    for (int i = 0; i < ROW_TOTAL; i++) {
        ll_value_init(&v_row[i], 0.0f, ll_spring_default);
    }
    ll_value_init(&v_pull, 0.0f, ll_spring_default);
    /* 条要跟着父层动的话，ll_value_set_parent(&child, &parent) 就行 */
    ll_value_set_precision(&v_pull, 0.05f);
}

/* ---------------- 事件 ---------------- */

static void lab_update(LlScene *self, float dt);

static int lab_key(LlScene *self, const LlKeyEvent *ev)
{
    (void)self;

    switch (ev->key) {
    case LL_KEY_LEFT:
        if (ev->phase == LL_PHASE_RELEASE) {
            move_bar(slot > 0 ? slot - 1 : BAR_SLOTS - 1);
            snprintf(status, sizeof(status), "cur %d/4", slot + 1);
        }
        return 1;
    case LL_KEY_RIGHT:
        if (ev->phase == LL_PHASE_RELEASE) {
            move_bar((slot + 1) % BAR_SLOTS);
            snprintf(status, sizeof(status), "cur %d/4", slot + 1);
        }
        return 1;
    case LL_KEY_UP:
        if (ev->phase == LL_PHASE_REPEAT) {
            if (focus_row == 0) {
                pull_to(pull_raw - 8.0f, "rubber");   /* 顶到顶还按：往反方向推 */
            } else {
                step_wave(focus_row - 1);
            }
            return 1;
        }
        if (ev->phase == LL_PHASE_PRESS) {
            return 1;
        }
        if (ev->phase == LL_PHASE_RELEASE) {
            if (focus_row == 0) {
                pull_to(0.0f, "released");
            } else {
                step_wave(focus_row - 1);
            }
        }
        return 1;
    case LL_KEY_DOWN:
        if (ev->phase == LL_PHASE_REPEAT) {
            /* 已经到底还继续按：整列被往那个方向推一下（橡皮筋） */
            if (focus_row >= ROW_TOTAL - 1) {
                pull_to(pull_raw + 8.0f, "rubber");
            } else {
                step_wave(focus_row + 1);
            }
            return 1;
        }
        if (ev->phase == LL_PHASE_PRESS) {
            return 1;          /* 按下只给反馈，动作在 Release/Repeat（lilt 的约定） */
        }
        if (ev->phase == LL_PHASE_RELEASE) {
            if (focus_row >= ROW_TOTAL - 1) {
                pull_to(0.0f, "released");     /* 顶到底：弹回 */
            } else {
                step_wave(focus_row + 1);
            }
        }
        return 1;
    case LL_KEY_OK:
        /* 按住不放 = 一路加深的超滚，松手 = 弹簧带回 0 */
        if (ev->phase == LL_PHASE_PRESS || ev->phase == LL_PHASE_REPEAT) {
            pull_to(pull_raw + 6.0f, "hold");
            return 1;
        }
        if (ev->phase == LL_PHASE_RELEASE) {
            pull_to(0.0f, "released");
        }
        return 1;
    case LL_KEY_MENU:
        if (ev->phase == LL_PHASE_RELEASE) {
            ll_value_bump(&v_edge_a, 2.0f);
            ll_value_bump(&v_edge_b, 2.0f);
            for (int i = 0; i < ROW_TOTAL; i++) {
                ll_value_bump(&v_row[i], 2.0f);
            }
            ll_value_bump(&v_pull, 3.0f);
            snprintf(status, sizeof(status), "bump");
        }
        return 1;
    case LL_KEY_BACK:
        if (ev->phase == LL_PHASE_RELEASE) {
            ll_demo_spring_reset();
            ll_app_invalidate();
            snprintf(status, sizeof(status), "reset");
        }
        return 1;
    default:
        return 0;
    }
}

/* ---------------- 绘制 ---------------- */

static void lab_paint(LlScene *self, LlCanvas canvas)
{
    LlTheme *t = &ll_theme;
    LlCanvas clip;
    LlRect   track, vp;
    float    a = ll_value_get(&v_edge_a);
    float    b = ll_value_get(&v_edge_b);
    float    pull_disp;
    int      shift;
    char     line[32];
    int      i;

    (void)self;

    /* 页头：左边标题，右边"在动的值有几个"和超滚读数 */
    ll_canvas_text(ll_canvas_translate(canvas, 2, HEAD_BASE), t->small_font, 0, 0,
                   "SPRING LAB", t->foreground);
    snprintf(line, sizeof(line), "n%d", ll_anim_active_count());
    ll_canvas_text(ll_canvas_translate(canvas, LL_SCREEN_W - 2 - 14, HEAD_BASE), t->small_font,
                   0, 0, line, t->foreground);
    ll_canvas_hline(canvas, 0, HEAD_LINE, LL_SCREEN_W, t->muted);

    /* 1 光标条：外框是轨道，实心块是光标。条的宽度 = b-a，会先变宽再收回来。 */
    track.x = BAR_X0;
    track.y = BAR_Y;
    track.w = BAR_X1 - BAR_X0;
    track.h = BAR_H;
    ll_canvas_stroke_round_rect(canvas, track, 3, t->muted);
    for (int s = 1; s < BAR_SLOTS; s++) {
        /* 槽位分隔只画上下两小段：单色屏没有"淡灰"，用形状表示次要 */
        ll_canvas_vline(canvas, (int16_t)(BAR_X0 + s * SLOT_W), (int16_t)(BAR_Y + 1), 1, t->muted);
        ll_canvas_vline(canvas, (int16_t)(BAR_X0 + s * SLOT_W),
                        (int16_t)(ll_rect_bottom(track) - 2), 1, t->muted);
    }
    if (b - a > 1.0f) {
        LlRect bar = { ll_round_f(a), (int16_t)(BAR_Y + 2),
                       (int16_t)(ll_round_f(b) - ll_round_f(a)), (int16_t)(BAR_H - 4) };
        ll_canvas_fill_round_rect(canvas, bar, 2, t->foreground);
    }

    /* 2 波浪行：视口裁剪，每行用自己的位移。
     * 超滚的显示位移 = ll_rubber_band(弹簧值)：弹簧给的是"想推多远"，
     * 橡皮筋把它压成"实际推多远"（越推越沉、永远到不了 PULL_DIM），
     * 所以整列会顶着一个越来越小的幅度往下躲，松手再弹回。 */
    pull_disp = ll_rubber_band(ll_value_get(&v_pull), PULL_DIM);
    shift     = ll_round_f(pull_disp);
    vp.x = LIST_X;
    vp.y = LIST_TOP;
    vp.w = LIST_W;
    vp.h = ROW_VIEW * ROW_H;
    clip = ll_canvas_clip(canvas, vp);
    for (i = 0; i < ROW_TOTAL; i++) {
        int16_t y = (int16_t)(LIST_TOP + i * ROW_H - scroll_row * ROW_H
                              + ll_round_f(ll_value_get(&v_row[i])) - shift);

        snprintf(line, sizeof(line), "Item %02d", i + 1);
        ll_canvas_text(clip, t->small_font, (int16_t)(LIST_X + 3),
                       (int16_t)(y + ROW_ASCENT), line, t->foreground);
    }
    /* 光标最后画：把已经画好的内容整块反色 => 白底黑字，字还留在原处。
     * lilt 的绘制顺序是"页面/弹窗 -> 光标 -> Toast"，这里照抄。 */
    {
        int16_t y = (int16_t)(LIST_TOP + focus_row * ROW_H - scroll_row * ROW_H
                              + ll_round_f(ll_value_get(&v_row[focus_row])) - shift);
        LlRect  cur = { LIST_X, y, LIST_W, ROW_H - 1 };

        if (ll_canvas_can_invert(canvas)) {
            ll_canvas_invert_round_rect(clip, cur, t->radius);
        } else {
            /* 没有反色能力的屏（彩色直写驱动）退化成描边，和 lilt 一致 */
            ll_canvas_stroke_round_rect(clip, cur, t->radius, t->accent);
        }
    }

    /* 3 底行：橡皮筋的实际显示位移（原始值 -> ll_rubber_band -> 弹簧） */
    snprintf(line, sizeof(line), "pull %+dpx", ll_round_f(pull_disp));
    ll_canvas_text(ll_canvas_translate(canvas, LL_SCREEN_W - 2 - 60, STATUS_BASE), t->small_font,
                   0, 0, line, t->muted);
    if (status[0]) {
        ll_canvas_text(ll_canvas_translate(canvas, 4, STATUS_BASE), t->small_font, 0, 0,
                       status, t->foreground);
    }
}

static void lab_update(LlScene *self, float dt)
{
    (void)self;
    (void)dt;
    /* 这里什么都不做：所有运动都由 ll_anim_step 推进，静止时零开销。 */
}

LlScene ll_demo_spring = {
    "spring-lab",
    lab_update,
    lab_paint,
    lab_key,
    0
};
