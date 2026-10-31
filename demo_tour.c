/* demo_tour.c — 自动巡演：一整圈动作排成一张表，帧循环照着它"自己敲按键"
 *
 * 为什么不直接调 API 去动界面：
 *   巡演只往事件环里塞按键（ll_input_emit），后面走的是和真人完全一样的路——
 *   ll_page_handle_key -> 焦点 -> 光标/波浪/页面栈。演示代码没有后门，
 *   你在屏幕上看到的每一帧，才等于你自己敲串口敲出来的那一帧。
 *
 * 时间表用"相对毫秒"累加，服务函数每帧被喂真实 dt，所以帧率掉一点只是整圈
 * 慢一点，动作之间的相对节奏不会变。连发那段的间隔刻意抄 KeyPoller 的规律
 * （0.2s 起手、0.07s 起每次 ×0.9、下限 0.03s），这样"按住"看上去就是按住。
 *
 * 屏上字幕只能是 ASCII：两套 tinyfont 只有 95 个可打印字符，没有汉字，
 * 中文写上去会画成一片空白。所以字幕英文、串口中文，两边各看各的。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "demo_tour.h"
#include "ll_anim.h"
#include "ll_app.h"
#include "ll_bus.h"
#include "ll_conf.h"
#include "ll_geom.h"
#include "ll_input.h"
#include "ll_node.h"
#include "ll_page.h"
#include "ll_theme.h"

#define ROWS        7
#define STEP_MAX    96
#define NO_SAY      (-1)

/* ---------------- 字幕：屏上短英文 + 串口长中文 ---------------- */

enum {
    CAP_READY, CAP_WAVE, CAP_CURSOR, CAP_PRESS, CAP_PAGEIN, CAP_SIDE,
    CAP_CLICK, CAP_BUMP, CAP_BACK, CAP_REPEAT, CAP_BAND, CAP_TOP,
    CAP_BOUNCY, CAP_CALM, CAP_PICK, CAP_MANUAL, CAP_LOOP, CAP_COUNT
};

/* 屏上的宽度有限：小字大约 6 像素一个字符，一页放得下 20 个。
 * host/main.c 里有一条自检逐条量过实际像素宽，超了会被点名。 */
static const char *const CAP_EN[CAP_COUNT] = {
    "auto tour k=manual",
    "1 wave: rows ripple",
    "2 cursor: 4 edges",
    "3 press is not fire",
    "4 page slides in",
    "5 left / right",
    "6 release fires it",
    "7 stuck: it bumps",
    "8 back: slides out",
    "9 hold: auto-repeat",
    "10 rubber band",
    "11 all the way up",
    "12 feel bouncy .45",
    "13 feel default .75",
    "picked (no-op row)",
    "manual: yours now",
    "14 loop again",
};

static const char *const CAP_ZH[CAP_COUNT] = {
    "巡演开始：接下来不用碰任何键，全部动作都是脚本往事件环里塞按键",
    "1 波浪 = 滚动位置瞬移，每行反向平移同样的量（这一帧看着没动），再各自滑回 0；离锚点越远滑得越慢",
    "2 光标不是一个点：上下左右四条边各有自己的弹簧，Lead 快 Trail 慢，所以它会朝目标拉长、到位再收拢",
    "3 按下只给视觉反馈（光标收缩、按钮整块反色），动作要等松手或连发才触发",
    "4 新页从屏幕下方推上来，旧页往上让出一小截，形成层叠感",
    "5 同一行里的两个按钮之间横跳：邻居判据是边到边的间隙，不会串到下一行",
    "6 松手才真的执行回调；回调只在下一帧开头的 do 队列里改界面，所以计数是“慢半帧”改的",
    "7 走不动了：没有滚动空间可顶，就给焦点那一行一个冲量，整列晃一下",
    "8 退场 = 往上滑出，同时下面那页回到正中；焦点回到这页记忆的位置",
    "9 按住连发：0.2 秒后起手，间隔 0.07 秒起、每次乘 0.9，越按越快",
    "10 到底再顶一下：只动 pull，它没有目标位移，橡皮筋曲线让它越推越沉、松手弹回",
    "11 反向一路按回顶部，波浪再来一次（这次是从下往上）",
    "12 MENU 改的是那支全局弹簧本身：阻尼 0.45，同一个动作过冲肉眼可见",
    "13 换回默认 0.75，再走一次对比一下",
    "这行没有实义，只是个普通列表项：巡演里它也真的被点到了",
    "检测到串口有输入，巡演停在这里，按键全部交还给你",
    "一整圈演完：退掉所有压在首页上的页、手感复位、焦点滚回第一行，重新开始",
};

int         ll_tour_caption_count(void) { return CAP_COUNT; }
const char *ll_tour_caption(int i)
{
    return (i >= 0 && i < CAP_COUNT) ? CAP_EN[i] : "";
}

/* ---------------- 页面上有哪些控件 ---------------- */

static const char *const ROW_NAME[ROWS] = {
    "Cursor + wave", "Repeat hold", "Rubber band", "Feel A/B",
    "Open detail", "Plain row", "Plain row 2"
};

/* 手感 A/B：只在这两档之间切，对比才看得清。 */
#define FEEL_B_R  0.29f
#define FEEL_B_D  0.75f
#define FEEL_A_R  0.25f
#define FEEL_A_D  0.45f

static LlNode *g_rows[ROWS];
static LlNode *g_cap_main;        /* 首页页脚字幕 */
static LlNode *g_cap_detail;      /* 详情页页脚字幕 */
static LlNode *g_count_node;
static char    g_count_text[24];
static int     g_count;
static int     g_bouncy;

static void open_detail(void *user)  { (void)user; ll_page_push(&ll_demo_tour_detail); }

static void pick_row(void *user)
{
    (void)user;
    if (g_cap_main) {
        ll_node_set_text(g_cap_main, CAP_EN[CAP_PICK]);
        ll_bus_log(CAP_ZH[CAP_PICK]);
    }
}

static void apply_count(void *user)
{
    int step = (int)(intptr_t)user;

    g_count += step;
    if (g_count > 99)  g_count = 99;
    if (g_count < -99) g_count = -99;
    /* %+04d：printf 的宽度算上正负号，所以 4 才是"符号 + 三位数字"。
     * 写 3 会印成 "+01"，字数一变标签就变宽，整页布局跟着抖。 */
    snprintf(g_count_text, sizeof(g_count_text), "count %+04d", g_count);
    ll_node_set_text(g_count_node, g_count_text);
}

static void on_plus(void *user)  { (void)user; ll_app_do(apply_count, (void *)(intptr_t)+1); }
static void on_minus(void *user) { (void)user; ll_app_do(apply_count, (void *)(intptr_t)-1); }

/* 页脚字幕是 root 的第二个孩子，不是列表里的行：
 * 滚动容器 grow=1 且主轴首选 0，所以它吃掉剩下的高度，这条 label 贴在屏幕底部，
 * 列表滚到哪都不会把它滚走；它跟着整页一起滑入滑出。 */
static LlNode *add_footer(LlPage *p, const char *text)
{
    LlNode *cap = ll_label_small(text);

    ll_set_padding_u(cap, ll_insets_xy(2, 0));
    ll_add(p->root, cap);
    return cap;
}

static void build_main(LlPage *p)
{
    g_cap_main = NULL;

    ll_page_put(p, ll_label_small("lilt-c auto tour"));
    for (int i = 0; i < ROWS; i++) {
        LlNode *it = ll_list_item(ROW_NAME[i],
                                  (i == 4) ? open_detail : pick_row, NULL);

        ll_set_padding_u(it, ll_insets_xy(3, 0));
        g_rows[i] = it;
        ll_page_put(p, it);
    }
    g_cap_main = add_footer(p, CAP_EN[CAP_READY]);
}

static void build_detail(LlPage *p)
{
    LlNode *row;
    LlNode *title;

    /* 每进一次这页都从 0 开始数：巡演一圈圈循环，计数留着会让下一圈对不上。
     * 真实应用里该把它存到页面外面（这里就是 g_count 变量本身）。 */
    g_count = 0;
    snprintf(g_count_text, sizeof(g_count_text), "count %+04d", g_count);

    title = ll_label("Detail");
    ll_set_padding_u(title, ll_insets_xy(3, 0));
    ll_page_put(p, title);

    g_count_node = ll_label(g_count_text);
    ll_set_padding_u(g_count_node, ll_insets_xy(3, 0));
    ll_page_put(p, g_count_node);

    row = ll_row();
    ll_set_gap_u(row, 4);
    row->main_align = 3;                        /* 两端对齐：两个按钮分居左右 */
    ll_add(row, ll_button("minus", on_minus, NULL));
    ll_add(row, ll_button("plus",  on_plus,  NULL));
    ll_set_padding_u(row, ll_insets_xy(2, 1));
    ll_page_put(p, row);

    ll_page_put(p, ll_label_small("press fills block"));
    ll_page_put(p, ll_label_small("release runs it"));
    ll_page_put(p, ll_label_small("back = j"));

    g_cap_detail = add_footer(p, CAP_EN[CAP_PAGEIN]);
}

/* 页面被弹掉之后它的节点就回收了，指针必须一起清掉：
 * 否则字幕会写进一个已经还给池子、可能已经被别的页占用的节点。 */
static void detail_leave(LlPage *p)
{
    (void)p;
    g_cap_detail = NULL;
    g_count_node = NULL;
}

/* MENU = 手感 A/B 开关。巡演里它是脚本按的，接管之后你按 k 也是它。 */
static int tour_key(LlPage *p, const LlKeyEvent *ev)
{
    (void)p;
    if (ev->key != LL_KEY_MENU || ev->phase != LL_PHASE_PRESS) {
        return 0;
    }
    g_bouncy = !g_bouncy;
    ll_spring_default->response = g_bouncy ? FEEL_A_R : FEEL_B_R;
    ll_spring_default->damping  = g_bouncy ? FEEL_A_D : FEEL_B_D;
    return 1;
}

LlPage ll_demo_tour_main = {
    .name = "tour", .build = build_main, .key = tour_key,
};
LlPage ll_demo_tour_detail = {
    .name = "tour detail", .build = build_detail, .leave = detail_leave,
};

/* ---------------- 时间轴 ---------------- */

typedef enum { ACT_EMIT = 0, ACT_SAY, ACT_LOOP } Act;

typedef struct {
    uint16_t at;        /* 距这一圈起点的毫秒 */
    uint8_t  act;
    int8_t   key;       /* ACT_EMIT 才用；LlKey 都 < 16，放得下 */
    uint8_t  phase;
    int8_t   say;       /* 字幕下标，NO_SAY = 不换 */
} Step;

static Step g_steps[STEP_MAX];
static int  g_n;
static int  g_truncated;         /* 表不够用了：自检里会被点名 */
static int  g_cursor_ms;      /* build 时间轴时的累加器 */
static int  g_next;           /* 下一个要执行的步骤下标 */
static int  g_ms;             /* 这一圈已经演到第几毫秒 */
static float g_ms_left;       /* 凑不满 1 毫秒的零头，攒着下一帧用 */
static int  g_running;

static void add(int gap, Act act, int key, int phase, int say)
{
    Step *s;

    if (g_n >= STEP_MAX) {
        g_truncated = 1;
        ll_bus_log("巡演步骤超了：加大 demo_tour.c 里的 STEP_MAX");
        return;
    }
    g_cursor_ms += gap;
    s       = &g_steps[g_n++];
    s->at   = (uint16_t)g_cursor_ms;
    s->act  = (uint8_t)act;
    s->key  = (int8_t)key;
    s->phase = (uint8_t)phase;
    s->say  = (int8_t)say;
}

/* 点按：按下 + 90ms 后松手。90ms 是串口那条链的松手判定窗口，照抄过来。 */
static void tap(int gap, LlKey k, int say)
{
    add(gap, ACT_EMIT, k, LL_PHASE_PRESS, say);
    add(90,  ACT_EMIT, k, LL_PHASE_RELEASE, NO_SAY);
}

/* 只按下，不松手：给调用方自己接 release_of()，用来演"按下给反馈、松手才触发"。 */
static void press(int gap, LlKey k, int say)
{
    add(gap, ACT_EMIT, k, LL_PHASE_PRESS, say);
}

static void release_of(int gap, LlKey k)
{
    add(gap, ACT_EMIT, k, LL_PHASE_RELEASE, NO_SAY);
}

/* 按住 n 次连发再松手，间隔规律和 ll_keypoll_init 里那五个常数一致。 */
static void hold(int gap, LlKey k, int n, int say)
{
    int   wait = 200;                  /* repeat_delay = 0.2s */
    float iv   = 0.07f;                /* repeat_interval = 0.07s */

    add(gap, ACT_EMIT, k, LL_PHASE_PRESS, say);
    for (int i = 0; i < n; i++) {
        add(wait, ACT_EMIT, k, LL_PHASE_REPEAT, NO_SAY);
        wait = (int)(iv * 1000.0f);
        iv *= 0.9f;                    /* accel */
        if (iv < 0.03f) {              /* min_interval */
            iv = 0.03f;
        }
    }
    add(90, ACT_EMIT, k, LL_PHASE_RELEASE, NO_SAY);
}

static void say_only(int gap, int say)
{
    add(gap, ACT_SAY, 0, 0, say);
}

/* ---------------- 一整圈的动作 ---------------- */

static void build_script(void)
{
    g_n = 0;
    g_cursor_ms = 0;

    /* 第一段：首页往下走焦。列表只有三行多点在视口里，所以每走一行都要滚，
     * 滚一次就是一次波浪；光标四条边各自追，走的时候会被拉长。 */
    say_only(350, CAP_READY);
    tap(430, LL_KEY_DOWN, CAP_WAVE);       /* 0 -> 1 */
    tap(430, LL_KEY_DOWN, CAP_CURSOR);     /* 1 -> 2 */
    tap(430, LL_KEY_DOWN, NO_SAY);         /* 2 -> 3 */

    /* 按下不做事，撑 320ms 让你看清按钮整块反色，松手才推页。
     * 跨页的字幕一律排在动作后面（say_only）：字幕写的是"当前在最上面那页"的页脚，
     * 而推页/出栈都发生在事件被帧循环取走之后，抢在前面写就写到要离开的那页上了。 */
    tap(420, LL_KEY_DOWN, NO_SAY);         /* 3 -> 4 = Open detail */
    press(220, LL_KEY_OK, CAP_PRESS);
    release_of(320, LL_KEY_OK);            /* 松手 = 推详情页 */
    say_only(400, CAP_PAGEIN);
    say_only(800, NO_SAY);

    /* 详情页：焦点落在第一个按钮上，横跳、点两下、再试试往下走（没路了）。 */
    tap(520, LL_KEY_RIGHT, CAP_SIDE);      /* minus -> plus */
    press(430, LL_KEY_OK, CAP_CLICK);
    release_of(300, LL_KEY_OK);            /* count +1 */
    press(420, LL_KEY_OK, NO_SAY);
    release_of(300, LL_KEY_OK);            /* count +2 */
    tap(520, LL_KEY_DOWN, CAP_BUMP);       /* 下面没有可聚焦的了：整列晃一下 */
    say_only(1000, NO_SAY);
    press(420, LL_KEY_BACK, NO_SAY);
    release_of(90, LL_KEY_BACK);           /* Back 也是松手才出栈 */
    say_only(400, CAP_BACK);               /* 字幕落在退回来的那一页上 */
    say_only(900, NO_SAY);

    /* 回到首页，焦点还记在"Open detail"那一行。按住连发一路到底，
     * 最后一发已经没路了，于是顺带顶出橡皮筋；再单点一下演得更清楚。 */
    hold(520, LL_KEY_DOWN, 2, CAP_REPEAT); /* 4 -> 5 -> 6 -> 顶住 */
    tap(700, LL_KEY_DOWN, CAP_BAND);
    say_only(1000, NO_SAY);
    hold(700, LL_KEY_UP, 5, CAP_TOP);      /* 6 -> 0 */
    say_only(900, NO_SAY);

    /* A/B：MENU 只改那支全局弹簧的两个数，界面一行代码都不知道有这回事。 */
    tap(400, LL_KEY_MENU, CAP_BOUNCY);
    tap(220, LL_KEY_DOWN, NO_SAY);
    tap(560, LL_KEY_DOWN, NO_SAY);
    say_only(900, NO_SAY);
    tap(300, LL_KEY_MENU, CAP_CALM);
    tap(220, LL_KEY_UP, NO_SAY);
    tap(560, LL_KEY_UP, NO_SAY);
    say_only(1200, NO_SAY);

    add(600, ACT_LOOP, 0, 0, CAP_LOOP);
}

/* 一圈演完：把状态恢复到“像刚上电”，但动画照常走，回卷本身也是能看的东西。 */
static void tour_rewind(void)
{
    while (ll_page_depth() > 1) {
        ll_page_pop();
    }
    g_bouncy = 0;
    ll_spring_default->response = FEEL_B_R;
    ll_spring_default->damping  = FEEL_B_D;
    if (g_rows[0]) {
        ll_page_focus(g_rows[0]);           /* 焦点回顶：这一段会滚一大截，波浪最大 */
    }
    g_ms      = 0;
    g_ms_left = 0.0f;
    g_next    = 0;
}

static void show_caption(int i)
{
    LlPage *p = ll_page_top();
    LlNode *cap = NULL;

    if (i < 0 || i >= CAP_COUNT) {
        return;
    }
    if (p == &ll_demo_tour_main) {
        cap = g_cap_main;
    } else if (p == &ll_demo_tour_detail) {
        cap = g_cap_detail;
    }
    if (cap) {
        ll_node_set_text(cap, CAP_EN[i]);   /* 内部已经 ll_app_invalidate() */
    }
    ll_bus_log(CAP_ZH[i]);
}

static void fire(const Step *s)
{
    /* 字幕先换、事件后入队：同一帧里帧循环才刚取完上一批事件，
     * 所以这一步的字幕描述的是"此刻屏幕上这一页正要做什么"。
     * 只有跨页的那两步例外，它们改用 say_only 排在动作后面。 */
    if (s->say != NO_SAY) {
        show_caption(s->say);
    }
    switch ((Act)s->act) {
    case ACT_EMIT:
        ll_input_emit((LlKey)s->key, (LlPhase)s->phase);
        break;
    case ACT_LOOP:
        tour_rewind();
        break;
    default:
        break;                               /* ACT_SAY：换字幕就算做完了 */
    }
}

void ll_tour_reset(void)
{
    g_truncated  = 0;
    build_script();
    g_next      = 0;
    g_ms        = 0;
    g_ms_left   = 0.0f;
    g_running   = 1;
    g_bouncy    = 0;
    g_cap_main  = NULL;
    g_cap_detail = NULL;
    g_count_node = NULL;
    memset(g_rows, 0, sizeof(g_rows));
    ll_spring_default->response = FEEL_B_R;
    ll_spring_default->damping  = FEEL_B_D;
}

void ll_tour_service(float dt)
{
    int inc;

    if (!g_running) {
        return;
    }
    g_ms_left += dt * 1000.0f;
    inc = (int)g_ms_left;
    if (inc <= 0) {
        return;                              /* 这一帧太快，还没攒够 1 毫秒 */
    }
    g_ms_left -= (float)inc;
    g_ms      += inc;

    while (g_next < g_n && (int)g_steps[g_next].at <= g_ms) {
        int looped = ((Act)g_steps[g_next].act == ACT_LOOP);

        fire(&g_steps[g_next]);
        if (looped) {
            return;          /* tour_rewind 已经把 g_ms / g_next 归零，本轮到此为止 */
        }
        g_next++;
    }
}

void ll_tour_take_over(void)
{
    if (!g_running) {
        return;
    }
    g_running = 0;
    show_caption(CAP_MANUAL);
}

int ll_tour_running(void) { return g_running; }

int ll_tour_total_ms(void)
{
    return g_n > 0 ? (int)g_steps[g_n - 1].at : 0;
}

int ll_tour_step_count(void) { return g_n; }

int ll_tour_truncated(void)  { return g_truncated; }
