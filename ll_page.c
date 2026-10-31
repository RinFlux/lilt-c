/* ll_page.c — 页面栈、进/出场、默认按键路由 */
#include <string.h>
#include "ll_page.h"
#include "ll_conf.h"
#include "ll_cursor.h"
#include "ll_anim.h"
#include "ll_bus.h"
#include "ll_theme.h"

static LlPage *g_stack[LL_PAGE_MAX];
static int     g_depth;
static LlPage *g_exiting;          /* 正在滑出的那一页，动画走完才回收 */

LlPage *ll_page_top(void)  { return g_depth > 0 ? g_stack[g_depth - 1] : NULL; }
int     ll_page_depth(void) { return g_depth; }

/* n 所在的滚动容器（沿祖先链往上找）。 */
static LlNode *scroller_for(LlNode *n);
static int     tree_contains(LlNode *root, LlNode *n);

static LlNode *scroller_for(LlNode *n)
{
    for (LlNode *p = n ? n->parent : NULL; p; p = p->parent) {
        if (p->kind == LL_NODE_SCROLL) {
            return p;
        }
    }
    return NULL;
}

static int tree_contains(LlNode *root, LlNode *n)
{
    if (!root || !n) {
        return 0;
    }
    if (root == n) {
        return 1;
    }
    if (!(root->flags & LL_F_VISIBLE)) {
        return 0;
    }
    for (int i = 0; i < root->n_kids; i++) {
        if (tree_contains(root->kids[i], n)) {
            return 1;
        }
    }
    return 0;
}

/* ---- 页壳子：Column -> Scroll -> 内容 Column ---- */
static void build_frame(LlPage *p)
{
    p->root = ll_column();
    ll_set_padding_u(p->root, (LlInsets){ 2, 1, 2, 1 });
    p->content  = ll_list();
    p->scroller = ll_scroll(p->content, 0);
    ll_add(p->root, p->scroller);
    p->gen   = ll_node_gen();
    p->focus = NULL;
}

void ll_page_put(LlPage *p, LlNode *n)
{
    if (p && n) {
        ll_add(p->content, n);
    }
}

void ll_page_set_content(LlPage *p, LlNode *n)
{
    LlScroll *d;

    if (!p || !n) {
        return;
    }
    d = ll_scroll_data(p->scroller);
    if (!d) {
        return;
    }
    p->content  = n;
    d->content  = n;
    d->anchor   = NULL;
    d->viewport = (LlRectF){ 0, 0, 0, 0 };
    ll_value_jump(&d->pos, 0.0f);
    ll_value_jump(&d->pull, 0.0f);
    /* 滚动容器只有"内容"这一个孩子：直接换掉它，不追加第二个 */
    p->scroller->kids[0] = n;
    p->scroller->n_kids  = 1;
    n->parent = p->scroller;
    ll_app_invalidate();
}

void ll_page_focus(LlNode *n)
{
    LlPage *p = ll_page_top();

    if (!n) {
        return;
    }
    ll_focus = n;
    if (p && n->gen == p->gen) {
        p->focus = n;                 /* 让这页记住焦点，回来时还在原地 */
    }
    ll_scroll_set_anchor(scroller_for(n), n);
    ll_node_reveal(n);
    ll_app_invalidate();
}

LlNode *ll_page_focused(void) { return ll_focus; }

LlNode *ll_page_focus_scroller(void) { return scroller_for(ll_focus); }

/* ---- 压栈 / 出栈 ---- */

static void remember_focus(LlPage *p)
{
    if (p && ll_focus && ll_focus->gen == p->gen) {
        p->focus = ll_focus;
    }
}

void ll_page_push(LlPage *p)
{
    LlPage *old = ll_page_top();

    if (!p) {
        return;
    }
    if (g_depth >= LL_PAGE_MAX) {
        ll_bus_log("页面栈满了：加大 LL_PAGE_MAX");
        return;
    }
    remember_focus(old);
    ll_node_set_gen(g_depth);
    build_frame(p);
    p->exiting = 0;
    if (p->build) {
        p->build(p);
    }
    g_stack[g_depth++] = p;

    /* 新页从屏幕下方推上来，旧页往上让出一小截，形成层叠感 */
    ll_value_init(&p->root->vy, (float)LL_SCREEN_H, ll_spring_smooth);
    ll_value_set(&p->root->vy, 0.0f);
    if (old) {
        ll_value_set(&old->root->vy, -(float)(LL_SCREEN_H / 8));
    }
    ll_focus = p->focus ? p->focus : ll_node_first_focusable(p->root);
    if (ll_focus) {
        ll_scroll_set_anchor(scroller_for(ll_focus), ll_focus);
        ll_node_reveal(ll_focus);
    } else {
        ll_cursor_show(0);
    }
    if (p->enter) {
        p->enter(p);
    }
    ll_app_invalidate();
}

void ll_page_pop(void)
{
    LlPage *p = ll_page_top();
    LlPage *below;

    if (g_depth <= 1 || !p) {
        return;                      /* 底层页不弹：退出由应用决定 */
    }
    if (p->leave) {
        p->leave(p);
    }
    g_depth--;
    below = g_stack[g_depth - 1];
    remember_focus(below);
    p->exiting = 1;
    ll_value_set_spring(&p->root->vy, ll_spring_smooth);
    ll_value_set(&p->root->vy, -(float)LL_SCREEN_H);   /* 往上退场 */
    ll_value_set(&below->root->vy, 0.0f);              /* 下面那页回到正中 */

    /* 焦点回到下面那页记忆的位置；记忆可能已经不在了，就退回第一个可聚焦节点 */
    ll_focus = (below->focus && tree_contains(below->root, below->focus))
             ? below->focus : ll_node_first_focusable(below->root);
    if (ll_focus) {
        ll_scroll_set_anchor(scroller_for(ll_focus), ll_focus);
        ll_node_reveal(ll_focus);
    }
    if (g_exiting) {
        /* 上一场退场还没走完就再次出栈：立刻回收旧的，池子不会漏 */
        ll_node_free_from(g_exiting->gen);
    }
    g_exiting = p;
    ll_cursor_press(0);
    ll_cursor_lean(0, 0);
    ll_app_invalidate();
}

void ll_page_replace(LlPage *p)
{
    LlPage *cur = ll_page_top();

    if (!cur || !p || p == cur) {
        return;
    }
    if (cur->leave) {
        cur->leave(cur);
    }
    g_depth--;
    ll_node_free_from(cur->gen);
    ll_page_push(p);
}

/* ---- 场景接口 ---- */

static void page_update(LlScene *self, float dt)
{
    (void)self;
    (void)dt;
    if (g_exiting && ll_value_settled(&g_exiting->root->vy)) {
        ll_node_free_from(g_exiting->gen);
        g_exiting->exiting = 0;
        g_exiting = NULL;
        ll_app_invalidate();
    }
}

static void paint_sheet(LlPage *p, LlCanvas canvas)
{
    int dy = ll_round_f(ll_value_get(&p->root->vy));
    LlRect sheet = { 0, (int16_t)dy, LL_SCREEN_W, LL_SCREEN_H };

    /* 每页先铺一层背景，这样推入/退场时是一块不透明的"页"在上面滑 */
    ll_canvas_fill_rect(canvas, sheet, ll_theme.background);
    ll_node_paint(p->root, canvas);
}

/* 从最上面往下找：哪一页已经整屏盖住了（dy 归零），它底下的页就一点都看不见，
 * 不用再画一遍。ESP32 上文字渲染是大头，这一条能省掉一半的过渡期开销。
 * 布局照旧全跑，省的是绘制——反正下一帧它露出来时位置得是对的。 */
static int first_visible(void)
{
    if (g_exiting && ll_round_f(ll_value_get(&g_exiting->root->vy)) == 0) {
        return g_depth;
    }
    for (int i = g_depth - 1; i >= 1; i--) {
        if (ll_round_f(ll_value_get(&g_stack[i]->root->vy)) == 0) {
            return i;
        }
    }
    return 0;
}

static void page_paint(LlScene *self, LlCanvas canvas)
{
    int first;

    (void)self;
    for (int i = 0; i < g_depth; i++) {
        ll_layout_tree(g_stack[i]->root, LL_SCREEN_W, LL_SCREEN_H);
    }
    if (g_exiting) {
        ll_layout_tree(g_exiting->root, LL_SCREEN_W, LL_SCREEN_H);
    }
    /* 光标目标在布局之后取，这样它追的是"刚摆好的位置" */
    if (ll_focus) {
        ll_cursor_move(ll_node_screen_rect(ll_focus));
    } else {
        ll_cursor_show(0);
    }
    first = first_visible();
    for (int i = first; i < g_depth; i++) {
        paint_sheet(g_stack[i], canvas);
    }
    if (g_exiting) {
        paint_sheet(g_exiting, canvas);
    }
    ll_cursor_paint(canvas);
}

/* 方向 -> 0=上 1=下 2=左 3=右 */
static int dir_of(LlKey k)
{
    switch (k) {
    case LL_KEY_UP:    return 0;
    case LL_KEY_DOWN:  return 1;
    case LL_KEY_LEFT:  return 2;
    case LL_KEY_RIGHT: return 3;
    default:           return -1;
    }
}

int ll_page_handle_key(const LlKeyEvent *ev)
{
    LlPage *p = ll_page_top();
    int     dir;

    if (!p || !ev) {
        return 0;
    }
    if (p->key && p->key(p, ev)) {
        return 1;
    }
    dir = dir_of(ev->key);
    if (dir >= 0) {
        LlNode *nx;

        if (ev->phase == LL_PHASE_RELEASE) {
            ll_cursor_lean(0, 0);
            return 1;
        }
        if (!ll_focus) {
            ll_focus = ll_node_first_focusable(p->root);
            if (!ll_focus) {
                return 1;
            }
        }
        /* 先朝按着的方向探出去几像素，再挪焦点：看上去像光标"想看那边" */
        ll_cursor_lean((dir == 3) - (dir == 2), (dir == 1) - (dir == 0));
        nx = ll_node_neighbor(p->root, ll_focus, dir);
        if (nx) {
            ll_page_focus(nx);
        } else {
            /* 走不动了：橡皮筋顶一下，到底还能感觉到"有东西" */
            LlNode *sc = ll_page_focus_scroller();
            LlRect  r  = ll_node_screen_rect(ll_focus);
            float   amt = (float)(r.h > 0 ? r.h : 12);

            if (sc) {
                ll_scroll_overscroll(sc, (dir == 1 || dir == 3) ? amt : -amt);
            } else {
                ll_value_bump(&ll_focus->vy, (dir == 1 || dir == 3) ? 3.0f : -3.0f);
            }
        }
        return 1;
    }

    switch (ev->key) {
    case LL_KEY_OK:
        if (ev->phase == LL_PHASE_PRESS) {
            if (ll_focus) {
                ll_focus->flags |= LL_F_PRESSED;
            }
            ll_cursor_press(1);
            return 1;
        }
        if (ll_event_fires(ev) && ll_focus) {
            LlClickFn fn = ll_node_click_of(ll_focus);
            /* 复选框自己翻转状态，回调里再读也行，这里先翻转再回调 */
            if (ll_focus->kind == LL_NODE_CHECKBOX) {
                ll_node_set_liked(ll_focus, !ll_node_liked(ll_focus));
            }
            if (fn) {
                fn(ll_node_user_of(ll_focus));
            }
        }
        if (ev->phase == LL_PHASE_RELEASE && ll_focus) {
            ll_focus->flags &= (uint8_t)~LL_F_PRESSED;
            ll_cursor_press(0);
        }
        return 1;

    case LL_KEY_BACK:
        /* 出栈只在松手时做一次：按住连发不会把整叠页一口气全弹光 */
        if (ev->phase == LL_PHASE_RELEASE && !ev->canceled) {
            ll_page_pop();
        }
        return 1;

    default:
        return 0;                     /* MENU / CHAR 交给应用 */
    }
}

static int scene_key(LlScene *self, const LlKeyEvent *ev)
{
    (void)self;
    return ll_page_handle_key(ev);
}

LlScene ll_page_scene = { "pages", page_update, page_paint, scene_key, NULL };

void ll_pages_boot(LlScreen *screen, LlPage *first)
{
    g_depth   = 0;
    g_exiting = NULL;
    ll_app_boot(screen, &ll_page_scene);   /* 先起帧循环：它会清空动画活跃表 */
    ll_nodes_init();
    ll_cursor_init();
    if (first) {
        ll_page_push(first);
    }
}
