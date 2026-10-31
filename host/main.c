/* main.c — 主机验收：数值自检 + 按脚本录帧（对应 lilt 的 cmd/preview）
 *
 *   gcc -o out/lab ../ll_*.c ../tf_*.c ../demo_spring.c host_bus.c main.c
 *   ./out/lab                 # 跑自检并写 out/frames.raw
 *   python render.py          # 出 out/spring-lab.gif 和 out/contact.png
 */
#ifdef ARDUINO
/* Arduino 会把库目录下所有 .c 一起收进编译。这个文件是 PC 侧的无头录帧 harness（自己实现了
 * ll_bus_*，还带一个 main()），在板子上必须整个编成空翻译单元，否则会和库根目录的
 * ll_board.cpp 撞符号、再多一个 main。它们只在 host/build.sh 那条链路里参与编译。 */
#else
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../demo_gallery.h"
#include "../demo_spring.h"
#include "../demo_tour.h"
#include "../ll_anim.h"
#include "../ll_app.h"
#include "../ll_conf.h"
#include "../ll_cursor.h"
#include "../ll_font.h"
#include "../ll_node.h"
#include "../ll_page.h"
#include "../ll_screen.h"
#include "../ll_theme.h"

void host_advance_ms(uint32_t ms);
int  host_snapshot(void);
void host_close(void);
void host_set_output(const char *path);

#define DT (1.0f / 30.0f)

static int g_fail;

static void check(const char *name, int ok, const char *fmt, ...)
{
    va_list ap;

    printf("%-30s %s  ", name, ok ? "OK" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!ok) {
        g_fail++;
    }
}

/* ---- 1. 弹簧：Default(0.29/0.75) 走 100 像素，应该过冲约 2.8%、几帧内收敛 ---- */
static void test_spring(void)
{
    LlValue v;
    int     frames = 0;
    float   max_over = 0.0f;

    ll_anim_init();
    ll_value_init(&v, 0.0f, ll_spring_default);
    ll_value_set(&v, 100.0f);
    while (ll_anim_step(DT) && frames < 300) {
        if (v.current - 100.0f > max_over) {
            max_over = v.current - 100.0f;
        }
        frames++;
    }
    /* 理论过冲 = exp(-pi*zeta/sqrt(1-zeta^2)) = 2.8% */
    check("spring settle frames", frames >= 8 && frames <= 60, "%d 帧 @30fps", frames);
    check("spring overshoot px", max_over > 1.0f && max_over < 6.0f, "%.2f / 100", (double)max_over);
    check("spring lands on target", ll_value_get(&v) == 100.0f, "%.3f", (double)ll_value_get(&v));
}

/* ---- 2. 纯缓动：Ease 不回弹，半个 Response 走完 1-1/e ---- */
static void test_ease(void)
{
    LlValue v;
    float   tau;
    float   max_over = 0.0f;
    int     frames   = 0;

    ll_anim_init();
    tau = 0.22f / 6.2831855f;            /* CursorLead 的 1/e 时间常数 */
    ll_value_init(&v, 0.0f, ll_spring_cursor_lead);
    ll_value_set(&v, 100.0f);
    ll_anim_step(tau);                   /* 正好一步：应关掉 1-1/e */
    check("ease: 1e fold per tau", ll_absf(ll_value_get(&v) - 63.2f) < 1.0f, "%.2f / 100",
          (double)ll_value_get(&v));
    /* Ease 是纯衰减，没有动量：无论跑多少帧都不该越过目标，且几帧内就该静止。
     * 用 while 的条件顺序也要注意——ll_anim_step 返回 0 表示全静止了。 */
    ll_anim_init();
    ll_value_init(&v, 0.0f, ll_spring_cursor_lead);
    ll_value_set(&v, 100.0f);
    while (frames < 300 && ll_anim_step(DT)) {
        if (v.current - 100.0f > max_over) {
            max_over = v.current - 100.0f;
        }
        frames++;
    }
    check("ease: never overshoots", max_over <= 0.0f, "%.4f px past target", (double)max_over);
    check("ease: settles", ll_value_settled(&v) && ll_value_get(&v) == 100.0f, "%d 帧 @30fps", frames);
}

/* ---- 3. expNeg 近似 vs 库函数 ---- */
static void test_expneg(void)
{
    float worst = 0.0f;
    float xs[] = { 0.0f, 0.05f, 0.3f, 0.6f, 1.2f, 3.0f, 7.5f, 15.0f };

    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        /* expNeg 是 static 的，这里用同样的调用路径检验：Ease 分支的衰减率 */
        float got, want = (float)exp(-(double)xs[i]);

        (void)got;
        /* 通过 ll_value 的 Ease 间接验证：一步之后的值应当 = x0 * exp(-rate*dt) */
        LlValue v;
        LlSpring s = { 1.0f, 1.0f, 1 };   /* rate = 2pi */
        float dt = xs[i] / 6.2831855f;

        ll_anim_init();
        ll_value_init(&v, 100.0f, &s);
        ll_value_set(&v, 0.0f);
        ll_anim_step(dt);
        got = ll_value_get(&v) / 100.0f;
        if (fabsf(got - want) > worst) {
            worst = fabsf(got - want);
        }
    }
    check("expNeg max abs error", worst < 2e-4f, "%.6f", (double)worst);
}

/* ---- 4. 橡皮筋：越推越沉，永远到不了 dim ---- */
static void test_rubber(void)
{
    float y1 = ll_rubber_band(10.0f, 10.0f);
    float y2 = ll_rubber_band(100.0f, 10.0f);
    float y3 = ll_rubber_band(10000.0f, 10.0f);

    check("rubber x=dim -> 0.355*dim", ll_absf(y1 - 3.548f) < 0.01f, "%.3f", (double)y1);
    check("rubber x=10*dim bounded", y2 < 10.0f && y2 > y1, "%.3f", (double)y2);
    check("rubber never reaches dim", y3 < 10.0f && y3 > y2, "%.3f", (double)y3);
    check("rubber is odd", ll_absf(ll_rubber_band(-10.0f, 10.0f) + y1) < 1e-5f, "%.3f", (double)y1);
}

/* ---- 5. 父值继承 + Shift + Kick ---- */
static void test_value_family(void)
{
    LlValue parent, child;

    ll_anim_init();
    ll_value_init(&parent, 0.0f, ll_spring_default);
    ll_value_init(&child, 10.0f, ll_spring_default);
    ll_value_set_parent(&child, &parent);
    ll_value_set(&parent, 50.0f);
    for (int i = 0; i < 200 && ll_anim_busy(); i++) {
        ll_anim_step(DT);
    }
    check("parent inherited in get", ll_value_get(&child) == 60.0f, "%.2f", (double)ll_value_get(&child));
    check("current excludes parent", ll_value_current(&child) == 10.0f, "%.2f",
          (double)ll_value_current(&child));

    ll_value_shift(&child, 5.0f);
    check("shift keeps target", ll_value_target(&child) == 10.0f && ll_value_current(&child) == 15.0f,
          "%.2f", (double)ll_value_current(&child));
    ll_value_kick(&child, -400.0f);
    ll_anim_step(DT);
    check("kick moves it", ll_value_current(&child) < 15.0f, "%.2f", (double)ll_value_current(&child));
}

/* ---- 6. 帧缓冲：填充、反色、脏页 ---- */
static void test_framebuffer(void)
{
    LlScreen *s = ll_screen_api();
    LlCanvas c  = ll_canvas_new(s);
    LlRect   r  = { 0, 0, 8, 8 };
    uint8_t  before, after;

    ll_screen_clear(ll_black);
    ll_screen_flush();
    ll_mono.buf[0] = 0;
    ll_canvas_fill_rect(c, r, ll_white);
    before = ll_mono.buf[0];
    ll_canvas_invert_rect(c, r);
    after = ll_mono.buf[0];
    check("fill then invert = 0", before == 0xFF && after == 0x00, "before=%02X after=%02X",
          (int)before, (int)after);

    ll_screen_clear(ll_black);
    ll_screen_flush();
    check("clean frame = no dirty pages", ll_screen_dirty_mask() == 0, "dirty=%d",
          (int)ll_screen_dirty_mask());

    /* 裁剪：画到屏幕外不应崩，也不应留下越界的像素。
     * 注意缓冲是"页主序、页内按列"：buf[page*W + x]，一个字节竖着管 8 个像素。
     * 所以第 2、3 列亮 = buf[2] 的 bit2|bit3 = 0x0C，而不是 buf[0]。 */
    ll_screen_clear(ll_black);
    ll_screen_flush();
    ll_canvas_fill_rect(ll_canvas_clip(c, (LlRect){ 0, 0, 4, 4 }), (LlRect){ 2, 2, 40, 40 }, ll_white);
    check("clip limits the fill",
          ll_mono.buf[2] == 0x0C && ll_mono.buf[3] == 0x0C &&
          ll_mono.buf[1] == 0x00 && ll_mono.buf[4] == 0x00 && ll_mono.buf[128] == 0x00,
          "col1=%02X col2=%02X col4=%02X page1=%02X", (int)ll_mono.buf[1], (int)ll_mono.buf[2],
          (int)ll_mono.buf[4], (int)ll_mono.buf[128]);

    /* 跨页：行 6..11 落在 page0 的上半（bit6、bit7）和 page1 的下半（bit0..3）。 */
    ll_screen_clear(ll_black);
    ll_canvas_fill_rect(c, (LlRect){ 0, 6, 1, 6 }, ll_white);
    check("fill spanning pages splits mask",
          ll_mono.buf[0] == 0xC0 && ll_mono.buf[128] == 0x0F,
          "page0=%02X page1=%02X", (int)ll_mono.buf[0], (int)ll_mono.buf[128]);

    /* 越界：负坐标和超出屏幕都要被吃掉，且不能碰坏缓冲。
     * (-10,-10,20,20) 覆盖 x 0..9、y 0..9：page0 的第 0..9 列整字节亮，
     * page1 的第 0..9 列只有 bit0|bit1 亮。 */
    ll_screen_clear(ll_black);
    ll_canvas_fill_rect(c, (LlRect){ -10, -10, 20, 20 }, ll_white);
    check("off-screen rect clips in",
          ll_mono.buf[0] == 0xFF && ll_mono.buf[9] == 0xFF && ll_mono.buf[10] == 0x00 &&
          ll_mono.buf[128] == 0x03 && ll_mono.buf[138] == 0x00,
          "p0c0=%02X p0c10=%02X p1c0=%02X", (int)ll_mono.buf[0], (int)ll_mono.buf[10],
          (int)ll_mono.buf[128]);

    /* 圆角：半径 3 的 8x8 块，角上应当缺像素 */
    ll_screen_clear(ll_black);
    ll_canvas_fill_round_rect(c, r, 3, ll_white);
    check("round rect cuts corner", (ll_mono.buf[0] & 0x01) == 0 && (ll_mono.buf[0] & 0x3C) != 0,
          "page0=%02X", (int)ll_mono.buf[0]);
}

/* ---- 7. 事件队列：满时丢弃、顺序不变、下标环绕后仍然正确 ---- */
static void test_input_queue(void)
{
    LlKeyEvent e;
    int emitted = 0;
    int got     = 0;

    ll_input_init();
    for (int i = 0; i < LL_INPUT_QUEUE + 5; i++) {
        if (ll_input_emit(LL_KEY_DOWN, LL_PHASE_PRESS)) {
            emitted++;
        }
    }
    check("queue drops when full", emitted == LL_INPUT_QUEUE - 1, "%d 入队 / 容量 %d",
          emitted, LL_INPUT_QUEUE);
    while (ll_input_pop(&e)) {
        got++;
    }
    check("queue drains in order", got == emitted, "%d 取出", got);
    check("empty after drain", ll_input_pop(&e) == 0 && ll_input_queued() == 0, "queued=%d",
          (int)ll_input_queued());

    /* 环绕：head/tail 绕回 0 之后入队出队仍要一一对应 */
    got = 0;
    for (int r = 0; r < 3 * LL_INPUT_QUEUE + 7; r++) {
        ll_input_emit(LL_KEY_OK, LL_PHASE_PRESS);
        if (ll_input_pop(&e) == 0 || e.key != LL_KEY_OK) {
            got = -1;
            break;
        }
    }
    check("wrap-around works", got == 0 && ll_input_queued() == 0, "queued=%d",
          (int)ll_input_queued());

    /* 动作触发规则：Press 只给反馈，Release 才动手；被取消的 Release 什么都不做 */
    {
        LlKeyEvent press   = { LL_KEY_OK, LL_PHASE_PRESS,   0u, 0 };
        LlKeyEvent rel     = { LL_KEY_OK, LL_PHASE_RELEASE, 0u, 0 };
        LlKeyEvent cancel  = { LL_KEY_OK, LL_PHASE_RELEASE, 0u, 1 };
        LlKeyEvent rep     = { LL_KEY_OK, LL_PHASE_REPEAT,  0u, 0 };

        check("press fires nothing",  !ll_event_fires(&press),  "");
        check("release fires",         ll_event_fires(&rel),    "");
        check("canceled release no",  !ll_event_fires(&cancel), "");
        check("repeat fires",          ll_event_fires(&rep),    "");
    }
}

/* ---- 8. 消抖与连发：喂电平进去，看发出来的事件序列对不对 ---- */
static void test_keypoll(void)
{
    LlKeyPoller p;
    LlKeyEvent  e;
    int         press = 0, release = 0, repeats = 0;

    ll_keypoll_init(&p);
    ll_input_init();
    /* 抖一下：单次采样不一致不该算按下（Debounce=2，要连续 3 次一致才认） */
    ll_keypoll_poll(&p, LL_KEY_DOWN, 1, 0.005f);
    ll_keypoll_poll(&p, LL_KEY_DOWN, 0, 0.005f);
    check("bounce ignored", ll_input_pop(&e) == 0, "");
    /* 稳定按下：前两次还不算，第三次才发 Press */
    ll_keypoll_poll(&p, LL_KEY_DOWN, 1, 0.005f);
    ll_keypoll_poll(&p, LL_KEY_DOWN, 1, 0.005f);
    check("press waits for debounce", ll_input_pop(&e) == 0, "");
    ll_keypoll_poll(&p, LL_KEY_DOWN, 1, 0.005f);
    check("debounced press", ll_input_pop(&e) && e.phase == LL_PHASE_PRESS, "");
    /* 按住 1 秒 -> 0.2 秒后开始连发，间隔 0.07 且每次 ×0.9，到 0.03 为止 */
    for (int i = 0; i < 200; i++) {
        ll_keypoll_poll(&p, LL_KEY_DOWN, 1, 0.005f);
    }
    while (ll_input_pop(&e)) {
        if (e.phase == LL_PHASE_REPEAT) {
            repeats++;
        }
    }
    check("auto-repeat started", repeats >= 3, "%d 次连发", repeats);
    ll_keypoll_poll(&p, LL_KEY_DOWN, 0, 0.005f);
    ll_keypoll_poll(&p, LL_KEY_DOWN, 0, 0.005f);
    check("release waits for debounce", ll_input_pop(&e) == 0, "");
    ll_keypoll_poll(&p, LL_KEY_DOWN, 0, 0.005f);
    while (ll_input_pop(&e)) {
        if (e.phase == LL_PHASE_RELEASE) {
            release++;
        }
    }
    check("release on debounce", release == 1, "%d 次松开", release);
    (void)press;
}

/* ================= B 阶段：节点树、滚动波浪、页面栈、光标 ================= */

/* ---- 9. 盒模型：竖排里按 measure 铺开，交叉轴拉伸，grow 抢剩余 ---- */
static void test_box_model(void)
{
    LlNode *root, *a, *b, *g;
    int     row_h;

    ll_nodes_init();
    root = ll_column();
    a    = ll_label("alpha");
    b    = ll_label("beta");
    g    = ll_label("grow me");
    ll_set_grow(g, 1.0f);
    ll_add(root, a);
    ll_add(root, b);
    ll_add(root, g);
    ll_layout_tree(root, LL_SCREEN_W, LL_SCREEN_H);

    row_h = ll_font_line_height(ll_theme.font) + 2;      /* RowPadding 上下各 1 */
    check("row height = line + pad", (int)a->outer.h == row_h, "%.0f / %d", a->outer.h, row_h);
    check("cross axis stretches", (int)a->outer.w == LL_SCREEN_W, "%.0f", a->outer.w);
    check("rows stack by height", (int)b->outer.y == row_h, "%.0f", b->outer.y);
    check("grow takes remainder", (int)g->outer.h == LL_SCREEN_H - 2 * row_h, "%.0f", g->outer.h);

    /* margin 把自己往里推，padding 把内容往里推 */
    ll_set_margin_u(a, ll_insets_all(4));
    ll_layout_tree(root, LL_SCREEN_W, LL_SCREEN_H);
    check("margin insets the box", (int)a->outer.x == 4 && (int)a->outer.w == LL_SCREEN_W - 8,
          "x=%.0f w=%.0f", a->outer.x, a->outer.w);
    /* margin 是算进首选高度的，所以下一行被整条 4+4 顶下去，不是只顶 4 */
    check("margin pushes siblings", (int)b->outer.y == row_h + 8, "%.0f / %d", b->outer.y,
          row_h + 8);
}

/* ---- 10. 滚动容器：内容跳、行错峰滑回，视觉在跳转瞬间不动 ---- */
static void test_scroll_wave(void)
{
    LlNode *root, *list, *sc, *rows[12];
    LlRect  before, at_jump, done;
    int     f_anchor = -1, f_far = -1;

    ll_nodes_init();
    ll_anim_init();
    root = ll_column();
    list = ll_list();
    sc   = ll_scroll(list, 0);
    ll_add(root, sc);
    for (int i = 0; i < 12; i++) {
        rows[i] = ll_list_item("Row", NULL, NULL);
        ll_add(list, rows[i]);
    }
    ll_layout_tree(root, LL_SCREEN_W, LL_SCREEN_H);

    check("scroll sees content size", ll_scroll_max(sc) > 40.0f, "%.0f px 可滚",
          (double)ll_scroll_max(sc));
    check("viewport is the screen", ll_scroll_data(sc)->view_size > 0.0f
          && ll_scroll_data(sc)->view_size < ll_scroll_data(sc)->content_size, "%.0f < %.0f",
          (double)ll_scroll_data(sc)->view_size, (double)ll_scroll_data(sc)->content_size);

    before = ll_node_screen_rect(rows[3]);
    ll_scroll_set_anchor(sc, rows[8]);
    ll_scroll_to(sc, 100.0f);
    at_jump = ll_node_screen_rect(rows[3]);
    check("wave holds pixels still", before.y == at_jump.y, "%d vs %d", before.y, at_jump.y);
    check("rows got shifted", ll_absf(ll_value_current(&rows[8]->vy) - 100.0f) < 0.01f,
          "%.1f", (double)ll_value_current(&rows[8]->vy));

    for (int i = 0; i < 200 && ll_anim_busy(); i++) {
        ll_anim_step(DT);
    }
    done = ll_node_screen_rect(rows[3]);
    check("rows glide to new place", done.y == before.y - 100, "%d vs %d", done.y, before.y - 100);
    check("wave settles to zero", ll_value_settled(&rows[0]->vy)
          && ll_absf(ll_value_current(&rows[0]->vy)) < 0.01f, "%.3f",
          (double)ll_value_current(&rows[0]->vy));

    /* 错峰：离锚点近的先归位。再来一次，这次逐帧盯两行的收敛速度。 */
    ll_scroll_to(sc, 40.0f);
    for (int i = 0; i < 120; i++) {
        ll_anim_step(DT);
        if (f_anchor < 0 && ll_absf(ll_value_current(&rows[8]->vy)) < 1.0f) {
            f_anchor = i;
        }
        if (f_far < 0 && ll_absf(ll_value_current(&rows[1]->vy)) < 1.0f) {
            f_far = i;
        }
    }
    check("anchor row leads the wave", f_anchor >= 0 && f_far > f_anchor, "%d < %d 帧",
          f_anchor, f_far);

    /* 橡皮筋：到底之后继续推，推出去的是显示位移，松手自己回弹 */
    ll_scroll_overscroll(sc, 30.0f);
    check("overscroll past the end", ll_scroll_pos(sc) > 40.5f && ll_scroll_pos(sc) < 70.0f,
          "%.1f", (double)ll_scroll_pos(sc));
    for (int i = 0; i < 120 && ll_anim_busy(); i++) {
        ll_anim_step(DT);
    }
    check("overscroll springs back", ll_absf(ll_scroll_pos(sc) - 40.0f) < 0.5f, "%.2f",
          (double)ll_scroll_pos(sc));
}

/* ---- 11. 焦点导航与命中 ---- */
static void test_navigation(void)
{
    LlNode *root, *list, *sc, *rows[6], *row_box, *l, *r;

    ll_nodes_init();
    ll_anim_init();
    root = ll_column();
    list = ll_column();
    sc   = ll_scroll(list, 0);
    ll_add(root, sc);
    for (int i = 0; i < 6; i++) {
        rows[i] = ll_list_item("item", NULL, NULL);
        ll_add(list, rows[i]);
    }
    /* 最下面放一行两个按钮：横向导航要有东西可走 */
    row_box = ll_row();
    l = ll_button("left", NULL, NULL);
    r = ll_button("right", NULL, NULL);
    ll_add(row_box, l);
    ll_add(row_box, r);
    ll_add(list, row_box);
    ll_layout_tree(root, LL_SCREEN_W, LL_SCREEN_H);

    check("first focusable is top row", ll_node_first_focusable(root) == rows[0], "%s",
          rows[0] == ll_node_first_focusable(root) ? "rows[0]" : "?");
    check("down goes to next row", ll_node_neighbor(root, rows[0], 1) == rows[1], "");
    check("up comes back", ll_node_neighbor(root, rows[2], 0) == rows[1], "");
    check("right hops a button", ll_node_neighbor(root, l, 3) == r, "");
    check("left hops back", ll_node_neighbor(root, r, 2) == l, "");
    check("no neighbor sideways from list", ll_node_neighbor(root, rows[0], 2) == NULL, "");
    check("tree order next", ll_node_next_focusable(root, rows[4]) == rows[5], "");
    check("tree order hits the button row", ll_node_next_focusable(root, rows[5]) == l, "");
    check("tree order wraps", ll_node_next_focusable(root, r) == rows[0], "");
    check("hit finds the row", ll_node_hit(root, 60, (int16_t)(rows[0]->outer.y + 4)) == rows[0],
          "y=%d", (int)rows[0]->outer.y + 4);
}

/* ---- 11b. 绘制落点：这两条都是"眼睛才看得见"的坑，用像素把它钉死 ----
 *
 * 缓冲的排布是 buf[page * W + x]，一个字节竖着管 8 行，所以"框外有没有点"
 * 只能按页对齐着判，纵向各留一页的余量。 */
static int lit_outside_box(LlRect allow)
{
    int x0 = allow.x - 1;
    int x1 = allow.x + allow.w;                 /* 不含 */
    int p0 = (allow.y - 8) / 8;
    int p1 = (allow.y + allow.h + 8) / 8;       /* 含 */
    int n  = 0;

    if (x0 < 0) {
        x0 = 0;
    }
    if (p0 < 0) {
        p0 = 0;
    }
    if (p1 >= LL_SCREEN_PAGES) {
        p1 = LL_SCREEN_PAGES - 1;
    }
    for (int p = 0; p < LL_SCREEN_PAGES; p++) {
        for (int x = 0; x < LL_SCREEN_W; x++) {
            int inside = (p >= p0 && p <= p1 && x >= x0 && x < x1);

            if (!inside && ll_mono.buf[p * LL_SCREEN_W + x]) {
                n++;
            }
        }
    }
    return n;
}

static int lit_from_page(int page)
{
    int n = 0;

    for (int p = page; p < LL_SCREEN_PAGES; p++) {
        for (int x = 0; x < LL_SCREEN_W; x++) {
            if (ll_mono.buf[p * LL_SCREEN_W + x]) {
                n++;
            }
        }
    }
    return n;
}

static void test_paint_bounds(void)
{
    LlScreen *s = ll_screen_api();
    LlCanvas  c;
    LlNode   *root, *list, *sc, *btn;
    LlRect    r;
    int       outside;

    ll_screen_init();
    ll_nodes_init();
    ll_anim_init();
    c = ll_canvas_new(s);

    /* 按钮离原点越远，"框画了两遍偏移"越藏不住：这里 outer 大约 (30,20)，
     * 画错就会落到 (60,40) 那一圈，正好在允许范围之外。 */
    root = ll_column();
    btn  = ll_button("tap", NULL, NULL);
    ll_set_margin_u(btn, ll_insets_xy(30, 20));
    ll_add(root, btn);
    ll_layout_tree(root, LL_SCREEN_W, LL_SCREEN_H);
    ll_screen_clear(ll_black);
    ll_node_paint(root, c);
    r       = ll_node_screen_rect(btn);
    outside = lit_outside_box(r);
    check("button frame stays in its rect", outside == 0 && r.x >= 30 && r.y >= 20,
          "%d 个亮点在框外 rect=%d,%d %dx%d", outside, r.x, r.y, r.w, r.h);

    /* 裁剪窗必须跟着页一起滑：整页往上推 40 像素后，视口也只剩屏幕上沿 24 行，
     * 视口外的行一点都不能从下面露出来（画错了就会亮到 page4 以下）。 */
    root = ll_column();
    list = ll_list();
    sc   = ll_scroll(list, 0);
    ll_add(root, sc);
    for (int i = 0; i < 10; i++) {
        ll_add(list, ll_label("row text"));
    }
    ll_layout_tree(root, LL_SCREEN_W, LL_SCREEN_H);
    ll_value_jump(&root->vy, -40.0f);
    ll_screen_clear(ll_black);
    ll_node_paint(root, c);
    check("clip rides with the page", lit_from_page(4) == 0, "%d 个亮点在视口下沿",
          lit_from_page(4));
}

/* ---- 12. 页面栈：进/出场、焦点记忆、整批回收、按键默认路由 ---- */
static int feed(LlKey k, LlPhase p)
{
    LlKeyEvent e = { k, p, 0u, 0 };

    return ll_page_handle_key(&e);
}

static void run_frames(int n)
{
    for (int i = 0; i < n; i++) {
        ll_app_step(DT);
    }
}

static const char *find_text(LlNode *n, const char *prefix)
{
    if (!n) {
        return NULL;
    }
    if (n->text && strncmp(n->text, prefix, strlen(prefix)) == 0) {
        return n->text;
    }
    for (int i = 0; i < n->n_kids; i++) {
        const char *t = find_text(n->kids[i], prefix);
        if (t) {
            return t;
        }
    }
    return NULL;
}

static LlNode *find_focusable(LlNode *root, const char *prefix)
{
    LlNode *n = ll_node_first_focusable(root);

    for (int i = 0; n && i < LL_NODE_MAX; i++) {
        if (n->text && strncmp(n->text, prefix, strlen(prefix)) == 0) {
            return n;
        }
        n = ll_node_next_focusable(root, n);
        if (n == ll_node_first_focusable(root)) {
            break;                       /* 绕了一圈还没找到 */
        }
    }
    return NULL;
}

static void test_pages(void)
{
    LlNode *sc, *nav;
    LlRect  fr, cr;
    float   before;
    int     used_after_main, used_after_detail;

    ll_screen_init();
    ll_pages_boot(ll_screen_api(), &ll_demo_gallery_main);
    run_frames(30);                       /* 首页推入的动画走完 */
    check("page stack depth 1", ll_page_depth() == 1, "%d", ll_page_depth());
    check("focus starts at first item", ll_focus
          && ll_focus->text && strcmp(ll_focus->text, "Detail page") == 0, "%s",
          ll_focus ? (ll_focus->text ? ll_focus->text : "(no text)") : "(none)");
    check("cursor is visible", ll_cursor_visible(), "");
    fr = ll_node_screen_rect(ll_focus);
    cr = ll_cursor_rect();
    check("cursor lands on focus", fr.y == cr.y && fr.h == cr.h, "cur %d/%d h %d/%d",
          cr.y, fr.y, cr.h, fr.h);
    used_after_main = ll_nodes_used();

    /* 方向键：Press 就挪，连发继续挪 */
    feed(LL_KEY_DOWN, LL_PHASE_PRESS);
    feed(LL_KEY_DOWN, LL_PHASE_RELEASE);
    check("down moves to Options", ll_focus && strcmp(ll_focus->text, "Options") == 0, "%s",
          ll_focus ? ll_focus->text : "(none)");
    for (int i = 0; i < 12; i++) {
        feed(LL_KEY_DOWN, i == 0 ? LL_PHASE_PRESS : LL_PHASE_REPEAT);
    }
    run_frames(2);
    sc = ll_page_focus_scroller();
    check("walked to the last row", ll_focus && strcmp(ll_focus->text, "Row 10") == 0, "%s",
          ll_focus ? ll_focus->text : "(none)");
    /* 等回弹彻底静止后再取一个基准值：下面要验证的是"松手回到刚才的位置"，
     * 而不是"回到内容底部"——Row 10 下面还有一行不可聚焦的状态文字，
     * reveal 把列表停在 164.5 就够了，max 175 是整列到底的位置。 */
    run_frames(90);
    before = ll_scroll_pos(sc);
    check("list scrolled for it", sc && before > 20.0f, "%.0f px", (double)before);
    /* 走过头：橡皮筋顶住，pos 不会越过 max，但显示位置会超出去一点 */
    for (int i = 0; i < 4; i++) {
        feed(LL_KEY_DOWN, LL_PHASE_REPEAT);
    }
    check("overscroll not past content", sc && ll_scroll_pos(sc) < ll_scroll_max(sc) + 20.0f,
          "%.0f / max %.0f", sc ? (double)ll_scroll_pos(sc) : 0.0,
          sc ? (double)ll_scroll_max(sc) : 0.0);
    check("overscroll actually moved", sc && ll_scroll_pos(sc) > before + 5.0f,
          "%.0f -> %.0f", (double)before, sc ? (double)ll_scroll_pos(sc) : 0.0);
    feed(LL_KEY_DOWN, LL_PHASE_RELEASE);
    run_frames(40);
    check("rubber retracted", sc && ll_absf(ll_scroll_pos(sc) - before) < 1.0f,
          "%.1f / %.1f", sc ? (double)ll_scroll_pos(sc) : 0.0, (double)before);

    /* OK：Press 只给反馈，动作在 Release 触发 */
    feed(LL_KEY_OK, LL_PHASE_PRESS);
    check("press sets PRESSED", (ll_focus->flags & LL_F_PRESSED) != 0, "%s", ll_focus->text);
    feed(LL_KEY_OK, LL_PHASE_RELEASE);
    check("release clears PRESSED", (ll_focus->flags & LL_F_PRESSED) == 0, "");
    run_frames(30);
    check("row click wrote status", find_text(ll_demo_gallery_main.root, "picked Row") != NULL,
          "%s", find_text(ll_demo_gallery_main.root, "picked") ?
              find_text(ll_demo_gallery_main.root, "picked") : "(none)");

    /* MENU：改的是那支全局弹簧 */
    feed(LL_KEY_MENU, LL_PHASE_PRESS);
    check("menu retunes springs", ll_absf(ll_spring_default->response - 0.39f) < 1e-4f,
          "response %.2f", (double)ll_spring_default->response);
    ll_gallery_reset_feel();

    /* 进详情页 -> 按钮 -> 计数（走 do 队列）。
     * 这里直接点名要聚焦的条目，不用连按 UP 往回走：往上走会经过整列 Row，
     * 走的步数和列表布局耦合得太紧，改一个行高就得重数按键。 */
    nav = find_focusable(ll_page_top()->root, "Detail page");
    check("nav item found", nav != NULL, "");
    ll_page_focus(nav);
    run_frames(20);
    feed(LL_KEY_OK, LL_PHASE_PRESS);
    feed(LL_KEY_OK, LL_PHASE_RELEASE);
    run_frames(40);
    check("detail pushed", ll_page_depth() == 2 && ll_page_top() == &ll_demo_gallery_detail,
          "depth %d", ll_page_depth());
    used_after_detail = ll_nodes_used();
    check("second page took nodes", used_after_detail > used_after_main, "%d -> %d",
          used_after_main, used_after_detail);
    check("focus is a button on detail", ll_focus && ll_focus->kind == LL_NODE_BUTTON, "%s",
          ll_focus && ll_focus->text ? ll_focus->text : "(none)");
    check("count starts at 000", find_text(ll_page_top()->root, "count +000") != NULL, "");
    /* 连点两次 plus：动作排进 do 队列，下一帧才改文本 */
    {
        LlNode *plus = ll_focus;
        LlNode *walk = ll_node_first_focusable(ll_page_top()->root);

        for (int i = 0; walk && i < 8; i++) {
            if (walk->text && strcmp(walk->text, "plus") == 0) {
                plus = walk;
                break;
            }
            walk = ll_node_next_focusable(ll_page_top()->root, walk);
        }
        ll_page_focus(plus);
        run_frames(20);
        feed(LL_KEY_OK, LL_PHASE_PRESS);
        feed(LL_KEY_OK, LL_PHASE_RELEASE);
        check("count frozen before frame", find_text(ll_page_top()->root, "count +000") != NULL, "");
        ll_app_step(DT);
        check("do-queue updated count", find_text(ll_page_top()->root, "count +001") != NULL,
              "%s", find_text(ll_page_top()->root, "count") ?
                  find_text(ll_page_top()->root, "count") : "(none)");
    }
    /* 出栈：先滑出，动画走完才回收节点 */
    feed(LL_KEY_BACK, LL_PHASE_PRESS);
    check("press does not pop", ll_page_depth() == 2, "%d", ll_page_depth());
    feed(LL_KEY_BACK, LL_PHASE_RELEASE);
    check("release pops", ll_page_depth() == 1, "%d", ll_page_depth());
    check("focus remembered on main", ll_focus && ll_focus->gen == 0, "%s",
          ll_focus && ll_focus->text ? ll_focus->text : "(none)");
    run_frames(60);
    check("exiting page recycled", ll_nodes_used() <= used_after_main + 2, "%d (main was %d)",
          ll_nodes_used(), used_after_main);

    /* 开关页：复选框被框架翻转，invert 真的换了前景/背景 */
    nav = find_focusable(ll_page_top()->root, "Options");
    check("options item found", nav != NULL, "");
    ll_page_focus(nav);
    run_frames(20);
    feed(LL_KEY_OK, LL_PHASE_PRESS);
    feed(LL_KEY_OK, LL_PHASE_RELEASE);
    run_frames(30);
    check("options pushed", ll_page_depth() == 2 && ll_page_top() == &ll_demo_gallery_options,
          "depth %d", ll_page_depth());
    check("focus is a checkbox", ll_focus && ll_focus->kind == LL_NODE_CHECKBOX, "%s",
          ll_focus && ll_focus->text ? ll_focus->text : "(none)");
    feed(LL_KEY_OK, LL_PHASE_PRESS);
    feed(LL_KEY_OK, LL_PHASE_RELEASE);
    check("invert swapped theme", ll_theme.foreground.r == 0, "fg=%d", (int)ll_theme.foreground.r);
    feed(LL_KEY_OK, LL_PHASE_PRESS);          /* 再点一次翻回来 */
    feed(LL_KEY_OK, LL_PHASE_RELEASE);
    check("invert swaps back", ll_theme.foreground.r == 255, "fg=%d", (int)ll_theme.foreground.r);
    feed(LL_KEY_BACK, LL_PHASE_RELEASE);
    run_frames(40);
    check("back at bottom is a no-op", ll_page_depth() == 1, "%d", ll_page_depth());
}

/* ---- 12. 自动巡演：脚本本身能自检的部分 ---- */
static void test_tour(void)
{
    int total, i, w, widest = 0, all_ascii = 1, frames, used_main;
    const char *wide = "";

    ll_tour_reset();
    total = ll_tour_total_ms();
    check("tour script fits its table",
          ll_tour_step_count() > 40 && !ll_tour_truncated(), "%d steps",
          ll_tour_step_count());
    /* 一整圈别太短（看不清动作）也别太长（没人有耐心看完）。 */
    check("one tour loop is 8..40 s", total >= 8000 && total <= 40000, "%d ms", total);

    for (i = 0; i < ll_tour_caption_count(); i++) {
        const unsigned char *s = (const unsigned char *)ll_tour_caption(i);
        for (const unsigned char *c = s; *c; c++) {
            if (*c < 0x20 || *c > 0x7e) {
                all_ascii = 0;      /* 那两套 tinyfont 只有 ASCII，汉字会画成空白 */
            }
        }
        w = ll_font_text_width(LL_FONT_SMALL, ll_tour_caption(i));
        if (w > widest) {
            widest = w;
            wide   = ll_tour_caption(i);
        }
    }
    check("tour captions are ASCII only", all_ascii, "%d captions", ll_tour_caption_count());
    /* 页脚左右各有内边距，再算上根容器的 padding，留 8 像素余量。 */
    check("tour captions fit the width", widest <= LL_SCREEN_W - 8,
          "widest %d px: \"%s\"", widest, wide);

    /* 连演两圈再查池子：巡演里页面反复进出，节点数应该稳稳回到首页那一版的量。 */
    ll_screen_init();
    ll_tour_reset();
    ll_pages_boot(ll_screen_api(), &ll_demo_tour_main);
    used_main = ll_nodes_used();
    frames    = (2 * total) / 33 + 60;
    for (i = 0; i < frames; i++) {
        ll_tour_service(DT);
        ll_app_step(DT);
    }
    check("two tour loops leave no nodes", ll_nodes_used() <= used_main,
          "%d <= %d after %d frames", ll_nodes_used(), used_main, frames);
    check("tour is still running", ll_tour_running(), "");
    check("focus stayed on page one", ll_focus && ll_focus->gen == 0,
          ll_focus && ll_focus->text ? ll_focus->text : "(none)");
    check("spring reset after A/B", ll_absf(ll_spring_default->damping - 0.75f) < 0.001f,
          "d=%.2f", (double)ll_spring_default->damping);
}

/* ---- 脚本：在固定帧号注入事件，正好录出可比对的动作 ---- */
typedef struct { int frame; LlKey key; LlPhase phase; } Ev;
static const Ev script[] = {
    /* 光标条：三次点按，看它拉长再收拢 */
    {   6, LL_KEY_RIGHT, LL_PHASE_PRESS   },
    {   9, LL_KEY_RIGHT, LL_PHASE_RELEASE },
    {  40, LL_KEY_RIGHT, LL_PHASE_PRESS   },
    {  43, LL_KEY_RIGHT, LL_PHASE_RELEASE },
    {  74, LL_KEY_LEFT,  LL_PHASE_PRESS   },
    {  77, LL_KEY_LEFT,  LL_PHASE_RELEASE },

    /* 按住 DOWN：连发往下走（间隔越来越短），走到最后一行后整列被橡皮筋顶住，
     * 松手弹回。 */
    { 100, LL_KEY_DOWN,  LL_PHASE_PRESS   },
    { 106, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 112, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 118, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 124, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 130, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 136, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 142, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 148, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 154, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 160, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 166, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 172, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 178, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 184, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 190, LL_KEY_DOWN,  LL_PHASE_REPEAT  },
    { 196, LL_KEY_DOWN,  LL_PHASE_RELEASE },

    /* 按住 UP：同样的动作反过来，看负方向的橡皮筋 */
    { 220, LL_KEY_UP,    LL_PHASE_PRESS   },
    { 226, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 232, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 238, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 244, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 250, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 256, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 262, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 268, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 274, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 280, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 286, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 292, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 298, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 304, LL_KEY_UP,    LL_PHASE_REPEAT  },
    { 310, LL_KEY_UP,    LL_PHASE_RELEASE },

    /* MENU：给所有值一记冲量，静止的东西也会弹一下再回去 */
    { 340, LL_KEY_MENU,  LL_PHASE_PRESS   },
    { 344, LL_KEY_MENU,  LL_PHASE_RELEASE },

    /* Back：复位 */
    { 380, LL_KEY_BACK,  LL_PHASE_PRESS   },
    { 384, LL_KEY_BACK,  LL_PHASE_RELEASE },
};
#define SCRIPT_N ((int)(sizeof(script) / sizeof(script[0])))

#define TOTAL_FRAMES 420

static void run_script(void)
{
    int next = 0;

    ll_screen_init();
    ll_demo_spring_reset();
    ll_app_boot(ll_screen_api(), &ll_demo_spring);

    for (int f = 0; f < TOTAL_FRAMES; f++) {
        while (next < SCRIPT_N && script[next].frame == f) {
            ll_input_emit(script[next].key, script[next].phase);
            next++;
        }
        ll_app_step(DT);
        host_snapshot();
        host_advance_ms(33);
    }
    host_close();
    printf("\n写出 out/frames.raw：%d 帧 x %d 字节\n", TOTAL_FRAMES,
           LL_SCREEN_W * LL_SCREEN_PAGES);
}

/* ---- B 阶段录帧：把相册应用从头演一遍，动作全部落在固定帧号上 ---- */
static Ev   g_gal[256];
static int  g_gal_n;
static int  g_gal_max;

static void ev_at(int frame, LlKey k, LlPhase p)
{
    if (g_gal_n < (int)(sizeof(g_gal) / sizeof(g_gal[0]))) {
        g_gal[g_gal_n++] = (Ev){ frame, k, p };
    }
    if (frame > g_gal_max) {
        g_gal_max = frame;
    }
}

/* 点一下：按下 6 帧后松开，中间不连发 */
static void tap(int frame, LlKey k)
{
    ev_at(frame, k, LL_PHASE_PRESS);
    ev_at(frame + 6, k, LL_PHASE_RELEASE);
}

/* 按住：press -> n 次 repeat -> release */
static void hold(int frame, LlKey k, int times, int step)
{
    ev_at(frame, k, LL_PHASE_PRESS);
    for (int i = 1; i <= times; i++) {
        ev_at(frame + i * step, k, LL_PHASE_REPEAT);
    }
    ev_at(frame + (times + 1) * step, k, LL_PHASE_RELEASE);
}

static void build_gallery_script(void)
{
    g_gal_n = 0;
    g_gal_max = 0;

    tap(36, LL_KEY_DOWN);            /* 光标拉长 -> 下一行 */
    tap(56, LL_KEY_DOWN);
    tap(76, LL_KEY_DOWN);
    hold(100, LL_KEY_DOWN, 24, 6);   /* 一路按到底：波浪 + 到底后的橡皮筋 */
    hold(300, LL_KEY_UP,   24, 6);   /* 反向再来一次 */
    tap(480, LL_KEY_OK);             /* 进详情页（整页从下面推上来） */
    tap(540, LL_KEY_RIGHT);          /* 两个按钮之间横跳 */
    tap(580, LL_KEY_OK);             /* plus -> 计数 +001 */
    tap(610, LL_KEY_OK);             /* plus -> 计数 +002 */
    tap(650, LL_KEY_DOWN);           /* 走不动：整列被顶一下 */
    tap(700, LL_KEY_BACK);           /* 详情页滑出去 */
    tap(780, LL_KEY_DOWN);           /* Options */
    tap(820, LL_KEY_OK);             /* 进开关页 */
    tap(880, LL_KEY_OK);             /* invert：前景背景互换 */
    tap(930, LL_KEY_DOWN);
    tap(950, LL_KEY_OK);             /* wide rows：首页每行变高 */
    tap(1000, LL_KEY_BACK);          /* 回到首页，行已经变宽、屏幕还是反的 */
    tap(1060, LL_KEY_OK);            /* 再点一次 invert，翻回来 */
    hold(1100, LL_KEY_DOWN, 6, 8);   /* 用变宽的行再看一遍波浪 */
    tap(1220, LL_KEY_MENU);          /* 换手感档位 */
    hold(1260, LL_KEY_DOWN, 6, 8);
}

static void run_gallery(void)
{
    int next = 0;

    build_gallery_script();
    host_set_output("out/frames-gallery.raw");
    ll_screen_init();
    ll_gallery_reset_feel();
    ll_theme.foreground = ll_white;
    ll_theme.background = ll_black;
    ll_pages_boot(ll_screen_api(), &ll_demo_gallery_main);

    for (int f = 0; f <= g_gal_max; f++) {
        while (next < g_gal_n && g_gal[next].frame == f) {
            ll_input_emit(g_gal[next].key, g_gal[next].phase);
            next++;
        }
        ll_app_step(DT);
        host_snapshot();
        host_advance_ms(33);
    }
    host_close();
    host_set_output("out/frames.raw");
    printf("写出 out/frames-gallery.raw：%d 帧，节点池峰值 %d\n", g_gal_max + 1, ll_nodes_used());
}

/* 巡演录帧：和板子上跑的是同一段代码——ll_tour_service(dt) + 帧循环。
 * 事件不是在这里按帧号注入的，而是巡演自己按毫秒发出来的，
 * 所以这段录出来的就是 Tour.ino 烧进板子后你会看到的东西。 */
static void run_tour(void)
{
    int total, frames;

    host_set_output("out/frames-tour.raw");
    ll_screen_init();
    ll_tour_reset();
    ll_pages_boot(ll_screen_api(), &ll_demo_tour_main);
    total = ll_tour_total_ms();

    /* dt = 1/30 -> 每帧推进 33.3 毫秒，多录 1.3 秒让回卷那段也进画面。 */
    frames = (total + 1300) * 30 / 1000;
    for (int f = 0; f < frames; f++) {
        ll_tour_service(DT);
        ll_app_step(DT);
        host_snapshot();
        host_advance_ms(33);
    }
    host_close();
    host_set_output("out/frames.raw");
    printf("写出 out/frames-tour.raw：%d 帧，一整圈 %d 毫秒\n", frames, total);
}

int main(void)
{
    printf("== lilt-c A 阶段自检 ==\n");
    test_spring();
    test_ease();
    test_expneg();
    test_rubber();
    test_value_family();
    ll_screen_init();
    test_framebuffer();
    test_input_queue();
    test_keypoll();
    printf("\n== lilt-c B 阶段自检 ==\n");
    test_box_model();
    test_scroll_wave();
    test_navigation();
    test_paint_bounds();
    test_pages();
    test_tour();
    printf("\n== 录帧：A 弹簧实验台 ==\n");
    run_script();
    printf("\n== 录帧：B 相册应用 ==\n");
    run_gallery();
    printf("\n== 录帧：自动巡演 ==\n");
    run_tour();
    printf("\n%s（失败 %d 项）\n", g_fail ? "有项目未通过" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}

#endif /* ARDUINO */
