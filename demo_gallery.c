/* demo_gallery.c — B 阶段验收应用
 *
 * 这里每一处都是"引擎的真实用法"，没有给演示特调的代码：
 *   列表  -> ll_scroll + 每行一根弹簧，波浪来自 ll_scroll_to 的错峰滑回
 *   光标  -> 四条边各自一根弹簧，跳页用 Leap，按住用 Release
 *   按钮  -> Press 只给反馈，动作在 Release/Repeat 触发（框架默认路由）
 *   计数  -> 模拟后台回调：只排队 ll_app_do，下一帧才动界面
 *   MENU  -> 直接改 ll_spring_default 这支可变全局弹簧，一处生效全树跟着变
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "demo_gallery.h"
#include "ll_conf.h"
#include "ll_cursor.h"
#include "ll_theme.h"

#define ROWS          10
#define DETAIL_LINES   5

/* 文案要用活得比节点久的缓冲区：节点只存指针，不复制字符串。 */
static char   g_row_text[ROWS][10];
static char   g_detail_text[DETAIL_LINES][16];
static char   g_status[24];
static char   g_count_text[20];

static LlNode *g_status_node;
static LlNode *g_count_node;
static LlNode *g_main_rows[ROWS];

static int g_count;
static int g_feel;
static int g_wide_rows;
static int g_inverted;
static int g_quiet;

/* ---- 手感档位：改的是那支全局弹簧本身 ---- */
static const struct { float response, damping; const char *name; } FEELS[] = {
    { 0.29f, 0.75f, "default" },
    { 0.39f, 0.85f, "smooth"  },
    { 0.31f, 0.80f, "snappy"  },
    { 0.25f, 0.45f, "bouncy"  },
};
#define FEEL_N ((int)(sizeof(FEELS) / sizeof(FEELS[0])))

void ll_gallery_reset_feel(void)
{
    g_feel  = 0;
    g_quiet = 0;
    ll_spring_default->response = FEELS[0].response;
    ll_spring_default->damping  = FEELS[0].damping;
}

static void say(const char *fmt, ...)
{
    va_list ap;

    if (!g_status_node) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    ll_node_set_text(g_status_node, g_status);
}

/* 页面底部那行小字提示。 */
static void add_hint(LlPage *p, const char *text)
{
    LlNode *h = ll_label_small(text);

    ll_set_padding_u(h, ll_insets_xy(3, 0));
    ll_page_put(p, h);
}

/* ---------------- 回调 ---------------- */

static void open_detail(void *user)  { (void)user; ll_page_push(&ll_demo_gallery_detail); }
static void open_options(void *user) { (void)user; ll_page_push(&ll_demo_gallery_options); }

static void pick_row(void *user)
{
    say("picked Row %02d", (int)(intptr_t)user + 1);
}

/* 真正的"后台改界面"：这里只排队，动界面在下一帧开头的 do 队列里做。 */
static void apply_count(void *user)
{
    int step = (int)(intptr_t)user;

    g_count += step;
    if (g_count > 999)  g_count = 999;
    if (g_count < -999) g_count = -999;
    /* %+04d：宽度算上正负号，所以 4 才是"符号 + 三位数字"，
     * 写成 %03d 会印成 "+01"，和初始文案 "count +000" 对不上。 */
    snprintf(g_count_text, sizeof(g_count_text), "count %+04d", g_count);
    ll_node_set_text(g_count_node, g_count_text);
}

static void on_plus(void *user)  { (void)user; ll_app_do(apply_count, (void *)(intptr_t)+1); }
static void on_minus(void *user) { (void)user; ll_app_do(apply_count, (void *)(intptr_t)-1); }

static void on_invert(void *user)
{
    LlColor b = ll_theme.background;

    (void)user;
    ll_theme.background = ll_theme.foreground;
    ll_theme.foreground = b;
    g_inverted = !g_inverted;
    ll_app_invalidate();
}

static void on_wide(void *user)
{
    (void)user;
    g_wide_rows = !g_wide_rows;
    /* 首页还压在下面那一层：直接改它每行的内边距，退回去时就变宽/变窄了 */
    for (int i = 0; i < ROWS; i++) {
        if (g_main_rows[i]) {
            ll_set_padding_u(g_main_rows[i], ll_insets_xy(3, g_wide_rows ? 2 : 0));
        }
    }
    ll_app_invalidate();
}

static void on_quiet(void *user)
{
    (void)user;
    g_quiet = !g_quiet;
    /* 又一处"全局手感"：阻尼拉到 0.95，整棵树的过冲几乎消失 */
    ll_spring_default->damping = g_quiet ? 0.95f : FEELS[g_feel].damping;
}

/* ---------------- 页：首页（列表 + 波浪） ---------------- */

static void build_main(LlPage *p)
{
    LlNode *head;

    g_status_node  = NULL;

    head = ll_label_small("lilt-c  B stage");
    ll_set_padding_u(head, ll_insets_xy(3, 0));
    ll_page_put(p, head);

    ll_page_put(p, ll_list_item("Detail page", open_detail, NULL));
    ll_page_put(p, ll_list_item("Options", open_options, NULL));

    for (int i = 0; i < ROWS; i++) {
        LlNode *it;

        snprintf(g_row_text[i], sizeof(g_row_text[i]), "Row %02d", i + 1);
        it = ll_list_item(g_row_text[i], pick_row, (void *)(intptr_t)i);
        ll_set_padding_u(it, ll_insets_xy(3, g_wide_rows ? 2 : 0));
        g_main_rows[i] = it;
        ll_page_put(p, it);
    }
    g_status_node = ll_label("w/s move  sp ok  j back");
    ll_set_padding_u(g_status_node, ll_insets_xy(3, 0));
    ll_page_put(p, g_status_node);
}

/* MENU 换手感；其余按键交给框架的默认路由。 */
static int main_key(LlPage *p, const LlKeyEvent *ev)
{
    (void)p;
    if (ev->key != LL_KEY_MENU || ev->phase != LL_PHASE_PRESS) {
        return 0;
    }
    g_feel = (g_feel + 1) % FEEL_N;
    ll_spring_default->response = FEELS[g_feel].response;
    if (!g_quiet) {
        ll_spring_default->damping = FEELS[g_feel].damping;
    }
    say("feel %s r=%.2f", FEELS[g_feel].name, (double)FEELS[g_feel].response);
    return 1;
}

/* ---------------- 页：详情（按钮 + 计数） ---------------- */

static void build_detail(LlPage *p)
{
    LlNode *row;
    LlNode *title = ll_label("Detail");

    ll_set_padding_u(title, ll_insets_xy(3, 0));
    ll_page_put(p, title);

    g_count_node = ll_label("count +000");
    ll_set_padding_u(g_count_node, ll_insets_xy(3, 0));
    ll_page_put(p, g_count_node);

    row = ll_row();
    ll_set_gap_u(row, 4);
    row->main_align = 3;                 /* 两端对齐：两个按钮分居左右 */
    ll_add(row, ll_button("minus", on_minus, NULL));
    ll_add(row, ll_button("plus",  on_plus,  NULL));
    ll_set_padding_u(row, ll_insets_xy(2, 1));
    ll_page_put(p, row);

    for (int i = 0; i < DETAIL_LINES; i++) {
        LlNode *l;

        snprintf(g_detail_text[i], sizeof(g_detail_text[i]), "line %d springs", i + 1);
        l = ll_label(g_detail_text[i]);
        ll_set_padding_u(l, ll_insets_xy(3, 0));
        ll_page_put(p, l);
    }
    add_hint(p, "ok = plus/minus  j = back");
}

/* ---------------- 页：开关 ---------------- */

static void build_options(LlPage *p)
{
    static const char *LABELS[3] = { "invert", "wide rows", "quiet feel" };
    static LlClickFn   fns[3]    = { on_invert, on_wide, on_quiet };
    int                init[3];

    /* 复选框的初始状态来自演示自己的变量：页面被回收重建也不会对不上 */
    init[0] = g_inverted;
    init[1] = g_wide_rows;
    init[2] = g_quiet;
    for (int i = 0; i < 3; i++) {
        LlNode *c = ll_check_box(LABELS[i], init[i], fns[i], NULL);

        ll_set_padding_u(c, ll_insets_xy(3, 0));
        ll_page_put(p, c);
    }
    g_status_node = NULL;               /* 这页不写首页那行状态 */
    add_hint(p, "ok = toggle  j = back");
}

/* 用指名初始化：框架字段留给 ll_page_push() 填，这里只写应用关心的部分。 */
LlPage ll_demo_gallery_main = {
    .name = "gallery", .build = build_main, .key = main_key,
};
LlPage ll_demo_gallery_detail = {
    .name = "detail", .build = build_detail,
};
LlPage ll_demo_gallery_options = {
    .name = "options", .build = build_options,
};
