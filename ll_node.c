/* ll_node.c — 节点池、盒模型、绘制与焦点导航
 *
 * 布局算法是"两遍法"：先自底向上量（measure），再自顶向下摆（layout）。
 * 这和 lilt 的 Box.Measure/SetSize 一一对应，只是把 Go 的接口调用换成了 switch。
 *
 * 坐标约定（重要，改这里的人先读这段）：
 *   - n->box   ：父容器分给它的绝对矩形，**含它自己的 margin**。
 *   - n->outer ：box 去掉 margin 后的绝对矩形 = 它的边框盒（border box）。
 *   - 绝对 = 屏幕坐标，未叠加任何动画位移；动画在绘制时才加上去。
 *   - pref_w/pref_h：measure 出来的"占用"，含 padding + margin，父容器直接拿它累加。
 */
#include <string.h>
#include "ll_node.h"
#include "ll_app.h"
#include "ll_bus.h"
#include "ll_conf.h"

/* ---------------- 池 ---------------- */

static LlNode  g_pool[LL_NODE_MAX];
static uint8_t g_used[LL_NODE_MAX];
static int     g_top;          /* 高水位，回收时往下退 */
static int     g_gen;

static LlScroll g_scroll_pool[LL_SCROLL_MAX];
static uint8_t  g_scroll_used[LL_SCROLL_MAX];

static LlScroll *scroll_alloc(void);
static void      scroll_free(const LlNode *n);

LlNode *ll_focus;

/* 每个节点最多一个点击回调，下标和节点池对齐（不 malloc 的代价）。 */
typedef struct { LlClickFn fn; void *user; } Click;
static Click g_clicks[LL_NODE_MAX];

void ll_nodes_init(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    memset(g_used, 0, sizeof(g_used));
    memset(g_clicks, 0, sizeof(g_clicks));
    memset(g_scroll_used, 0, sizeof(g_scroll_used));
    g_top = 0;
    g_gen = 0;
    ll_focus = NULL;
}

int ll_nodes_used(void)   { return g_top; }
int ll_node_gen(void)     { return g_gen; }
void ll_node_set_gen(int gen) { g_gen = gen; }

static int node_index(const LlNode *n) { return (int)(n - g_pool); }

LlNode *ll_node_new(LlNodeKind kind)
{
    int i;

    for (i = 0; i < LL_NODE_MAX; i++) {
        if (!g_used[i]) {
            LlNode *n = &g_pool[i];

            memset(n, 0, sizeof(*n));
            g_used[i] = 1;
            if (i >= g_top) {
                g_top = i + 1;
            }
            n->kind        = kind;
            n->gen         = (uint8_t)g_gen;
            n->flags       = LL_F_VISIBLE;
            n->cross_align = 3;         /* 交叉轴默认拉伸，和 lilt 的 Box 一样 */
            ll_value_init(&n->vx, 0.0f, ll_spring_default);
            ll_value_init(&n->vy, 0.0f, ll_spring_default);
            return n;
        }
    }
    ll_bus_log("节点池满了：加大 LL_NODE_MAX");
    return NULL;
}

/* ---------------- 滚动数据池 ---------------- */

static LlScroll *scroll_alloc(void)
{
    for (int i = 0; i < LL_SCROLL_MAX; i++) {
        if (!g_scroll_used[i]) {
            g_scroll_used[i] = 1;
            memset(&g_scroll_pool[i], 0, sizeof(g_scroll_pool[i]));
            return &g_scroll_pool[i];
        }
    }
    ll_bus_log("滚动容器用完了：加大 LL_SCROLL_MAX");
    return NULL;
}

static void scroll_free(const LlNode *n)
{
    if (n->user) {
        int i = (int)((LlScroll *)n->user - g_scroll_pool);
        if (i >= 0 && i < LL_SCROLL_MAX) {
            g_scroll_used[i] = 0;
        }
    }
}

void ll_node_free_from(int gen)
{
    for (int i = 0; i < g_top; i++) {
        if (g_used[i] && g_pool[i].gen >= gen) {
            if (g_pool[i].kind == LL_NODE_SCROLL) {
                scroll_free(&g_pool[i]);
            }
            g_clicks[i].fn   = NULL;
            g_clicks[i].user = NULL;
            g_used[i] = 0;
            /* 焦点可能正好在被回收的页上 */
            if (ll_focus == &g_pool[i]) {
                ll_focus = NULL;
            }
        }
    }
    while (g_top > 0 && !g_used[g_top - 1]) {
        g_top--;
    }
}

/* ---------------- 建树 ---------------- */

void ll_add(LlNode *parent, LlNode *child)
{
    if (!parent || !child) {
        return;
    }
    if (parent->n_kids >= LL_NODE_MAX_KIDS) {
        ll_bus_log("一个容器的孩子满了：加大 LL_NODE_MAX_KIDS");
        return;
    }
    child->parent  = parent;
    parent->kids[parent->n_kids++] = child;
}

/* 单位 -> 像素。主题字体下单位 = 1 像素，换大字体时整套间距一起放大。 */
static float u2p(float units) { return units * ll_unit(); }

void ll_set_padding_u(LlNode *n, LlInsets units) { n->pad = ll_insets_scale(units, ll_unit()); }
void ll_set_margin_u(LlNode *n, LlInsets units)  { n->mar = ll_insets_scale(units, ll_unit()); }
void ll_set_gap_u(LlNode *n, float units)        { n->gap_px = u2p(units); }

void ll_set_fixed_size(LlNode *n, int16_t w, int16_t h)
{
    n->fix_w = w;
    n->fix_h = h;
}

void ll_set_grow(LlNode *n, float g) { n->grow = g; }

void ll_set_flags(LlNode *n, uint8_t on, uint8_t off)
{
    n->flags = (uint8_t)((n->flags & ~off) | on);
}

void ll_animate_layout(LlNode *n) { n->flags |= LL_F_ANIM_POS; }

void ll_node_set_text(LlNode *n, const char *text)
{
    if (n) {
        n->text = text;
        ll_app_invalidate();
    }
}

void ll_node_set_liked(LlNode *n, int liked)
{
    if (n) {
        ll_set_flags(n, liked ? LL_F_LIKED : 0, liked ? 0 : LL_F_LIKED);
        ll_app_invalidate();
    }
}

int ll_node_liked(const LlNode *n) { return n && (n->flags & LL_F_LIKED) ? 1 : 0; }

/* ---------------- 便捷构造 ---------------- */

static const LlFont *pick_font(const LlNode *n)
{
    return n->font ? n->font : ll_theme.font;
}

LlNode *ll_label(const char *text)
{
    LlNode *n = ll_node_new(LL_NODE_LABEL);

    if (n) {
        n->text = text;
        ll_set_padding_u(n, ll_theme.row_padding);
    }
    return n;
}

LlNode *ll_label_small(const char *text)
{
    LlNode *n = ll_label(text);

    if (n) {
        n->font = ll_theme.small_font;
    }
    return n;
}

static void set_click(LlNode *n, LlClickFn fn, void *user)
{
    int i = node_index(n);

    g_clicks[i].fn   = fn;
    g_clicks[i].user = user;
}

LlClickFn ll_node_click_of(const LlNode *n)
{
    return (n && n >= g_pool && n < g_pool + LL_NODE_MAX) ? g_clicks[node_index(n)].fn : NULL;
}

void *ll_node_user_of(const LlNode *n)
{
    return (n && n >= g_pool && n < g_pool + LL_NODE_MAX) ? g_clicks[node_index(n)].user : NULL;
}

LlNode *ll_button(const char *text, LlClickFn on_click, void *user)
{
    LlNode *n = ll_node_new(LL_NODE_BUTTON);

    if (n) {
        n->text  = text;
        n->flags |= LL_F_FOCUSABLE;
        ll_set_padding_u(n, ll_theme.button_padding);
        set_click(n, on_click, user);
    }
    return n;
}

LlNode *ll_list_item(const char *text, LlClickFn on_click, void *user)
{
    LlNode *n = ll_node_new(LL_NODE_LIST_ITEM);

    if (n) {
        n->text  = text;
        n->flags |= LL_F_FOCUSABLE;
        ll_set_padding_u(n, ll_theme.row_padding);
        set_click(n, on_click, user);
    }
    return n;
}

LlNode *ll_check_box(const char *text, int checked, LlClickFn on_toggle, void *user)
{
    LlNode *n = ll_node_new(LL_NODE_CHECKBOX);

    if (n) {
        n->text  = text;
        n->flags |= LL_F_FOCUSABLE | (checked ? LL_F_LIKED : 0);
        ll_set_padding_u(n, ll_theme.row_padding);
        ll_set_gap_u(n, 4);
        set_click(n, on_toggle, user);
    }
    return n;
}

LlNode *ll_column(void)
{
    LlNode *n = ll_node_new(LL_NODE_BOX);

    if (n) {
        n->dir = LL_DIR_COLUMN;
    }
    return n;
}

LlNode *ll_row(void)
{
    LlNode *n = ll_node_new(LL_NODE_BOX);

    if (n) {
        n->dir = LL_DIR_ROW;
        n->cross_align = 0;      /* 行里的东西默认顶对齐，不拉伸 */
    }
    return n;
}

LlNode *ll_list(void)
{
    LlNode *n = ll_column();

    if (n) {
        ll_set_gap_u(n, ll_theme.gap);
        ll_set_padding_u(n, ll_insets_xy(2, 1));
    }
    return n;
}

/* ---------------- 滚动容器 ---------------- */

LlScroll *ll_scroll_data(const LlNode *n)
{
    return (n && n->kind == LL_NODE_SCROLL) ? (LlScroll *)n->user : NULL;
}

LlNode *ll_scroll(LlNode *content, int horizontal)
{
    LlNode   *s = ll_node_new(LL_NODE_SCROLL);
    LlScroll *d;

    if (!s) {
        return NULL;
    }
    d = scroll_alloc();
    if (!d) {
        g_used[node_index(s)] = 0;
        return NULL;
    }
    d->content    = content;
    d->horizontal = horizontal ? 1 : 0;
    ll_value_init(&d->pos,  0.0f, ll_spring_default);
    ll_value_init(&d->pull, 0.0f, ll_spring_default);
    s->user = d;
    s->grow = 1.0f;                  /* 视口吃满父容器在主轴上剩下的空间 */
    if (content) {
        ll_add(s, content);
    }
    return s;
}

/* 波浪用的弹簧表：lilt 把 response 按"离焦点第几行"拉长，
 * 于是近处的行马上归位、远处的行慢半拍，一串涟漪就出来了。
 * 每次用时按当前的 ll_spring_default 重建，改总控参数照样全局生效。 */
static LlSpring g_wave[8];

static void build_wave_springs(void)
{
    for (int k = 0; k < 8; k++) {
        g_wave[k].response = ll_spring_default->response * (1.0f + 0.2f * (float)k);
        g_wave[k].damping  = ll_spring_default->damping;
        g_wave[k].ease     = 0;
    }
}

float ll_scroll_max(const LlNode *n)
{
    const LlScroll *d = ll_scroll_data(n);
    float mx;

    if (!d) {
        return 0.0f;
    }
    mx = d->content_size - d->view_size;
    return mx > 0.0f ? mx : 0.0f;
}

/* 显示位置 = 滚动位置 + 橡皮筋处理过的超滚量：越推越沉，松手弹回。 */
float ll_scroll_pos(const LlNode *n)
{
    const LlScroll *d = ll_scroll_data(n);
    float dim;

    if (!d) {
        return 0.0f;
    }
    dim = d->horizontal ? d->viewport.w : d->viewport.h;
    return ll_value_get(&d->pos) + ll_rubber_band(ll_value_get(&d->pull), dim > 1.0f ? dim : 1.0f);
}

void ll_scroll_set_anchor(LlNode *n, LlNode *anchor)
{
    LlScroll *d = ll_scroll_data(n);

    if (d) {
        d->anchor = anchor;
    }
}

void ll_scroll_to(LlNode *n, float pos)
{
    LlScroll *d = ll_scroll_data(n);
    float     mx, delta, old;
    int       anchor_i = -1, i;

    if (!d || !d->content) {
        return;
    }
    mx    = ll_scroll_max(n);
    pos   = ll_clampf(pos, 0.0f, mx);
    old   = ll_value_get(&d->pos);
    delta = pos - old;
    if (ll_absf(delta) < 0.5f) {
        return;                      /* 半个像素以内不值得惊动任何人 */
    }
    build_wave_springs();
    ll_value_jump(&d->pos, pos);     /* 内容"跳"过去 */

    /* 每行反向平移同样的量：这一帧看上去什么都没动；
     * 之后各自滑回 0，离锚点越远滑得越慢——错峰就是这么来的。 */
    for (i = 0; i < d->content->n_kids; i++) {
        if (d->content->kids[i] == d->anchor) {
            anchor_i = i;
            break;
        }
    }
    for (i = 0; i < d->content->n_kids; i++) {
        LlNode *kid = d->content->kids[i];
        int     k;

        if (!kid || !(kid->flags & LL_F_VISIBLE)) {
            continue;
        }
        k = (anchor_i < 0) ? 0 : (i > anchor_i ? i - anchor_i : anchor_i - i);
        if (k > 7) {
            k = 7;
        }
        ll_value_set_spring(&kid->vy, &g_wave[k]);
        ll_value_shift(&kid->vy, delta);
    }
    ll_app_invalidate();
}

void ll_scroll_shift(LlNode *n, float d)
{
    const LlScroll *s = ll_scroll_data(n);

    if (s) {
        ll_scroll_to(n, ll_value_get(&s->pos) + d);
    }
}

/* 到底/到顶之后继续推：只动 pull，它没有目标位移，松手就弹回。 */
void ll_scroll_overscroll(LlNode *n, float d)
{
    LlScroll *s = ll_scroll_data(n);

    if (!s) {
        return;
    }
    ll_value_set_spring(&s->pull, ll_spring_default);
    ll_value_shift(&s->pull, d);
    ll_app_invalidate();
}

/* ---------------- measure ---------------- */

static void finish_measure(LlNode *n, float border_w, float border_h)
{
    if (n->fix_w > 0) {
        border_w = (float)n->fix_w;
    }
    if (n->fix_h > 0) {
        border_h = (float)n->fix_h;
    }
    n->pref_w = (int16_t)(border_w + ll_insets_horizontal(n->mar) + 0.5f);
    n->pref_h = (int16_t)(border_h + ll_insets_vertical(n->mar) + 0.5f);
}

static void measure_leaf(LlNode *n)
{
    const LlFont *f = pick_font(n);
    float tw = f ? (float)ll_font_text_width(f, n->text ? n->text : "") : 0.0f;
    float lh = f ? (float)ll_font_line_height(f) : 8.0f;
    float bw = tw + ll_insets_horizontal(n->pad);
    float bh = lh + ll_insets_vertical(n->pad);

    if (n->kind == LL_NODE_CHECKBOX) {
        bw += lh + n->gap_px;                    /* 左边一个见方的框 */
    } else if (n->kind == LL_NODE_LIST_ITEM) {
        bw += (float)ll_round_f(lh * 0.5f) + 4;  /* 右边的 ">" */
    }
    finish_measure(n, bw, bh);
}

static int kid_visible(const LlNode *k)
{
    return k && (k->flags & LL_F_VISIBLE);
}

static void measure_flex(LlNode *n)
{
    int    horizontal = (n->dir == LL_DIR_ROW);
    float  main = 0.0f, cross = 0.0f;
    float  bw, bh;
    int    cnt = 0;

    for (int i = 0; i < n->n_kids; i++) {
        LlNode *k = n->kids[i];
        float   km, kc;

        if (!kid_visible(k)) {
            continue;
        }
        ll_node_measure(k);
        km = horizontal ? (float)k->pref_w : (float)k->pref_h;
        kc = horizontal ? (float)k->pref_h : (float)k->pref_w;
        if (cnt > 0) {
            main += n->gap_px;
        }
        main += km;
        if (kc > cross) {
            cross = kc;
        }
        cnt++;
    }
    bw = horizontal ? main : cross;
    bh = horizontal ? cross : main;
    bw += ll_insets_horizontal(n->pad);
    bh += ll_insets_vertical(n->pad);
    finish_measure(n, bw, bh);
}

static void measure_scroll(LlNode *n)
{
    LlScroll *d = ll_scroll_data(n);
    float     bw = 0.0f, bh = 0.0f;

    if (d && d->content) {
        float cm, cc;

        ll_node_measure(d->content);
        cm = d->horizontal ? (float)d->content->pref_w : (float)d->content->pref_h;
        cc = d->horizontal ? (float)d->content->pref_h : (float)d->content->pref_w;
        d->content_size = cm;
        /* 主轴首选 0：滚动容器愿意被压成任意大小的视口，交叉轴跟内容走 */
        bw = d->horizontal ? 0.0f : cc;
        bh = d->horizontal ? cc : 0.0f;
    }
    bw += ll_insets_horizontal(n->pad);
    bh += ll_insets_vertical(n->pad);
    finish_measure(n, bw, bh);
}

void ll_node_measure(LlNode *n)
{
    if (!n) {
        return;
    }
    if (!(n->flags & LL_F_VISIBLE)) {
        n->pref_w = 0;
        n->pref_h = 0;
        return;
    }
    switch (n->kind) {
    case LL_NODE_BOX:
        measure_flex(n);
        break;
    case LL_NODE_SCROLL:
        measure_scroll(n);
        break;
    default:
        measure_leaf(n);
        break;
    }
}

/* ---------------- layout ---------------- */

static LlRectF inset_margin(LlRectF r, LlInsets m)
{
    r.x += m.left;
    r.y += m.top;
    r.w -= m.left + m.right;
    r.h -= m.top + m.bottom;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

/* 内容盒 = 边框盒往里缩一个 padding（pad 存的是正的"内缩"，和 lilt 一样）。 */
static LlRectF inset_padding(LlRectF r, LlInsets p)
{
    return inset_margin(r, p);
}

static void layout_flex(LlNode *n, LlRectF inner)
{
    LlNode *ks[LL_NODE_MAX_KIDS];
    float   size[LL_NODE_MAX_KIDS];
    int     horizontal = (n->dir == LL_DIR_ROW);
    int     cnt = 0;
    float   avail      = horizontal ? inner.w : inner.h;
    float   cross_avail = horizontal ? inner.h : inner.w;
    float   used = 0.0f, sum_grow = 0.0f, extra, leftover, cursor, stretch = 0.0f;

    for (int i = 0; i < n->n_kids && cnt < LL_NODE_MAX_KIDS; i++) {
        LlNode *k = n->kids[i];
        float   km;

        if (!kid_visible(k)) {
            continue;
        }
        ks[cnt++] = k;
        km = horizontal ? (float)k->pref_w : (float)k->pref_h;
        /* grow 的孩子也先按自身首选尺寸占位，剩下的空间再按 grow 比例分给它。
         * 和 measure_flex 保持一致：首选尺寸是所有孩子相加，这样父容器被拉大时，
         * 多出来的部分才会完整地落到 grow 孩子头上，而不是被算两遍。 */
        used += km;
        if (k->grow > 0.0f) {
            sum_grow += k->grow;
        }
    }
    if (cnt > 1) {
        used += n->gap_px * (float)(cnt - 1);
    }
    extra = avail - used;
    if (extra < 0.0f) {
        extra = 0.0f;
    }
    for (int i = 0; i < cnt; i++) {
        LlNode *k = ks[i];
        float   km = horizontal ? (float)k->pref_w : (float)k->pref_h;

        size[i] = (k->grow > 0.0f && sum_grow > 0.0f)
                ? km + extra * k->grow / sum_grow
                : km;
    }
    leftover = avail;
    for (int i = 0; i < cnt; i++) {
        leftover -= size[i];
    }
    leftover -= (cnt > 1) ? n->gap_px * (float)(cnt - 1) : 0.0f;
    if (leftover < 0.0f) {
        leftover = 0.0f;
    }
    if (sum_grow <= 0.0f) {
        switch (n->main_align) {
        case 1:  cursor = leftover * 0.5f; break;
        case 2:  cursor = leftover;        break;
        case 3:  cursor = 0.0f;
                 if (cnt > 1) stretch = leftover / (float)(cnt - 1);
                 break;
        default: cursor = 0.0f; break;
        }
    } else {
        cursor = 0.0f;
    }

    for (int i = 0; i < cnt; i++) {
        LlNode  *k = ks[i];
        float    kc = horizontal ? (float)k->pref_h : (float)k->pref_w;
        float    m  = cursor;
        float    coff, csize;
        LlRectF  kr;

        if (n->cross_align == 3) {
            csize = cross_avail;
            coff  = 0.0f;
        } else {
            csize = kc;
            switch (n->cross_align) {
            case 1:  coff = (cross_avail - kc) * 0.5f; break;
            case 2:  coff = cross_avail - kc;          break;
            default: coff = 0.0f; break;
            }
            if (coff < 0.0f) coff = 0.0f;
        }
        if (horizontal) {
            kr.x = inner.x + m;      kr.y = inner.y + coff;
            kr.w = size[i];          kr.h = csize;
        } else {
            kr.x = inner.x + coff;   kr.y = inner.y + m;
            kr.w = csize;            kr.h = size[i];
        }
        ll_node_layout(k, kr);
        cursor += size[i] + n->gap_px + stretch;
    }
}

static void layout_scroll(LlNode *n, LlRectF inner)
{
    LlScroll *d = ll_scroll_data(n);
    LlRectF   cr;
    float     mx;

    if (!d) {
        return;
    }
    d->viewport  = inner;
    d->view_size = d->horizontal ? inner.w : inner.h;
    if (!d->content) {
        d->content_size = 0.0f;
        return;
    }
    ll_node_measure(d->content);
    d->content_size = d->horizontal ? (float)d->content->pref_w : (float)d->content->pref_h;

    /* 内容按它自己的完整主轴尺寸铺开，交叉轴拉伸到视口宽度 */
    cr.x = inner.x;
    cr.y = inner.y;
    if (d->horizontal) {
        cr.w = d->content_size;
        cr.h = inner.h;
    } else {
        cr.w = inner.w;
        cr.h = d->content_size;
    }
    ll_node_layout(d->content, cr);

    /* 内容或视口变短了，把位置夹回来，别留下白边 */
    mx = d->content_size - d->view_size;
    if (mx < 0.0f) {
        mx = 0.0f;
    }
    ll_value_set(&d->pos, ll_clampf(ll_value_target(&d->pos), 0.0f, mx));
}

void ll_node_layout(LlNode *n, LlRectF box)
{
    LlRectF inner;

    if (!n) {
        return;
    }
    n->box   = box;
    n->outer = inset_margin(box, n->mar);
    if (!(n->flags & LL_F_VISIBLE)) {
        return;
    }
    inner = inset_padding(n->outer, n->pad);

    switch (n->kind) {
    case LL_NODE_BOX:
        if (n->dir == LL_DIR_NONE) {
            for (int i = 0; i < n->n_kids; i++) {
                if (kid_visible(n->kids[i])) {
                    ll_node_layout(n->kids[i], inner);   /* 无主轴 = 叠在一起 */
                }
            }
        } else {
            layout_flex(n, inner);
        }
        break;
    case LL_NODE_SCROLL:
        layout_scroll(n, inner);
        break;
    default:
        break;                                          /* 叶子：记下矩形就够了 */
    }
}

void ll_layout_tree(LlNode *root, int16_t w, int16_t h)
{
    LlRectF box;

    if (!root) {
        return;
    }
    ll_node_measure(root);
    box = (LlRectF){ 0.0f, 0.0f, (float)w, (float)h };
    ll_node_layout(root, box);
}

/* ---------------- 绘制 ---------------- */

static LlRect pixel_rect(LlRectF r) { return ll_rectf_pixels(r); }

static void paint_label(const LlNode *n, LlCanvas own)
{
    LlRect r = pixel_rect(n->outer);
    const LlFont *f = pick_font(n);
    LlCanvas clip = ll_canvas_clip(own, (LlRect){ 0, 0, r.w, r.h });
    int16_t  base_y;

    if (!f) {
        return;
    }
    base_y = (int16_t)(ll_round_f(n->pad.top) + ll_font_ascent(f));
    ll_canvas_text(clip, f, (int16_t)ll_round_f(n->pad.left), base_y,
                   n->text ? n->text : "", ll_theme.foreground);
}

static void paint_button(const LlNode *n, LlCanvas own)
{
    LlRect r = pixel_rect(n->outer);
    int16_t rad = (int16_t)ll_round_f(u2p((float)ll_theme.radius));
    const LlFont *f = pick_font(n);
    int16_t tw, base_y, tx;

    /* own 的原点已经是这个节点的左上角，所以框要画在本地 (0,0)：
     * 直接把 outer 当矩形传进去会把它加两遍，整颗按钮飞出屏幕外。 */
    r.x = 0;
    r.y = 0;
    if (n->flags & LL_F_PRESSED) {
        /* 单色屏上的"按下"：整块填满，字用不亮的颜色画成洞 */
        ll_canvas_fill_round_rect(own, r, rad, ll_theme.foreground);
    } else {
        ll_canvas_stroke_round_rect(own, r, rad, ll_theme.foreground);
    }
    if (!f) {
        return;
    }
    tw     = ll_font_text_width(f, n->text ? n->text : "");
    tx     = (int16_t)((r.w - tw) / 2);
    base_y = (int16_t)((r.h - ll_font_line_height(f)) / 2 + ll_font_ascent(f));
    ll_canvas_text(own, f, tx, base_y, n->text ? n->text : "",
                   (n->flags & LL_F_PRESSED) ? ll_theme.background : ll_theme.foreground);
}

/* 右侧的 ">"：两个短线，比画个三角省笔墨，也够指路。 */
static void paint_chevron(LlCanvas c, int16_t cx, int16_t cy, int16_t s, LlColor col)
{
    ll_canvas_line(c, cx, cy - s, (int16_t)(cx + s), cy, col);
    ll_canvas_line(c, cx + s, cy, cx, (int16_t)(cy + s), col);
}

static void paint_list_item(const LlNode *n, LlCanvas own)
{
    LlRect r = pixel_rect(n->outer);
    const LlFont *f = pick_font(n);
    int16_t  s, cx, cy;

    paint_label(n, own);
    if (!f) {
        return;
    }
    s  = (int16_t)(ll_font_line_height(f) / 4);
    if (s < 2) {
        s = 2;
    }
    cy = (int16_t)(r.h / 2);
    cx = (int16_t)(r.w - ll_round_f(n->pad.right) - 2 * s);
    paint_chevron(own, cx, cy, s, ll_theme.foreground);
}

static void paint_check_box(const LlNode *n, LlCanvas own)
{
    LlRect          r = pixel_rect(n->outer);
    const LlFont   *f = pick_font(n);
    int16_t         side, x, base_y;
    LlCanvas        t;

    if (!f) {
        return;
    }
    side    = (int16_t)ll_font_line_height(f);
    if (side + 2 > r.h) {
        side = (int16_t)(r.h - 2);
    }
    x       = (int16_t)ll_round_f(n->pad.left);
    base_y  = (int16_t)(ll_round_f(n->pad.top) + ll_font_ascent(f));
    t       = ll_canvas_translate(own, x, (int16_t)((r.h - side) / 2));

    ll_canvas_stroke_round_rect(t, (LlRect){ 0, 0, side, side }, 2, ll_theme.foreground);
    if (n->flags & LL_F_LIKED) {
        /* 一个对勾：两段线，比画个实心块省电也更好认 */
        ll_canvas_line(t, 2, side / 2, (int16_t)(side / 2), (int16_t)(side - 3), ll_theme.foreground);
        ll_canvas_line(t, (int16_t)(side / 2), (int16_t)(side - 3), (int16_t)(side - 2), 2,
                       ll_theme.foreground);
    }
    /* 文字接在框右边，裁到盒子内 */
    {
        int16_t  tx = (int16_t)(x + side + ll_round_f(n->gap_px));
        LlCanvas tc = ll_canvas_clip(ll_canvas_translate(own, tx, 0),
                                     (LlRect){ 0, 0,
                                               (int16_t)(r.w - tx - ll_round_f(n->pad.right)), r.h });
        ll_canvas_text(tc, f, 0, base_y, n->text ? n->text : "", ll_theme.foreground);
    }
}

void ll_node_paint(LlNode *n, LlCanvas canvas)
{
    int       dx, dy;
    LlCanvas  self;

    if (!n || !(n->flags & LL_F_VISIBLE)) {
        return;
    }
    dx   = ll_round_f(ll_value_get(&n->vx));
    dy   = ll_round_f(ll_value_get(&n->vy));
    self = ll_canvas_translate(canvas, (int16_t)dx, (int16_t)dy);

    if (n->kind == LL_NODE_SCROLL) {
        LlScroll *d = ll_scroll_data(n);
        LlRect    vp;
        LlCanvas  view;
        int       po;

        if (!d || !d->content) {
            return;
        }
        vp   = pixel_rect(d->viewport);
        /* viewport 存的是"布局绝对坐标"，而 ll_canvas_clip 的入参是本地坐标、
         * 内部会再加上画布自己的偏移。self 的偏移正是祖先们累计的动画位移，
         * 一加一减刚好抵消，所以这里直接传 vp 就是对的：
         * 视口在屏幕上就躺在 vp + 累计位移 处。
         * （早先这里多减了一次 self.ox/oy，整页滑动时裁剪窗跟着页一起跑偏，
         *   退场那页超出视口的行就画到上一页身上去了。） */
        view = ll_canvas_clip(self, vp);
        po   = ll_round_f(ll_scroll_pos(n));
        view = d->horizontal ? ll_canvas_translate(view, (int16_t)-po, 0)
                             : ll_canvas_translate(view, 0, (int16_t)-po);
        for (int i = 0; i < n->n_kids; i++) {
            ll_node_paint(n->kids[i], view);
        }
        return;
    }

    /* 画自己：把原点推到节点的边框盒左上角（本地 0,0 = 左上角） */
    {
        LlCanvas own = ll_canvas_translate(self, (int16_t)ll_round_f(n->outer.x),
                                           (int16_t)ll_round_f(n->outer.y));
        switch (n->kind) {
        case LL_NODE_LABEL:     paint_label(n, own);      break;
        case LL_NODE_BUTTON:    paint_button(n, own);     break;
        case LL_NODE_LIST_ITEM: paint_list_item(n, own);  break;
        case LL_NODE_CHECKBOX:  paint_check_box(n, own);  break;
        default:                break;
        }
    }
    /* 再画孩子：孩子自己会加自己的绝对位置与动画 */
    for (int i = 0; i < n->n_kids; i++) {
        ll_node_paint(n->kids[i], self);
    }
}

/* ---------------- 焦点导航 ---------------- */

static int focusable(const LlNode *n)
{
    return n && (n->flags & (LL_F_VISIBLE | LL_F_FOCUSABLE)) == (LL_F_VISIBLE | LL_F_FOCUSABLE);
}

static LlNode *g_walk[LL_NODE_MAX];
static int     g_walk_n;

static void collect_focusable(LlNode *n)
{
    if (!n || g_walk_n >= LL_NODE_MAX) {
        return;
    }
    if (!(n->flags & LL_F_VISIBLE)) {
        return;
    }
    if (focusable(n)) {
        g_walk[g_walk_n++] = n;
    }
    for (int i = 0; i < n->n_kids; i++) {
        collect_focusable(n->kids[i]);
    }
}

LlNode *ll_node_first_focusable(LlNode *root)
{
    g_walk_n = 0;
    collect_focusable(root);
    return g_walk_n > 0 ? g_walk[0] : NULL;
}

LlNode *ll_node_next_focusable(LlNode *root, LlNode *from)
{
    g_walk_n = 0;
    collect_focusable(root);
    for (int i = 0; i < g_walk_n; i++) {
        if (g_walk[i] == from) {
            return g_walk[(i + 1) % g_walk_n];
        }
    }
    return g_walk_n > 0 ? g_walk[0] : NULL;
}

LlNode *ll_node_prev_focusable(LlNode *root, LlNode *from)
{
    g_walk_n = 0;
    collect_focusable(root);
    for (int i = 0; i < g_walk_n; i++) {
        if (g_walk[i] == from) {
            return g_walk[(i + g_walk_n - 1) % g_walk_n];
        }
    }
    return g_walk_n > 0 ? g_walk[g_walk_n - 1] : NULL;
}

LlRect ll_node_screen_rect(LlNode *n)
{
    int x, y;

    if (!n) {
        return (LlRect){ 0, 0, 0, 0 };
    }
    x = ll_round_f(n->outer.x);
    y = ll_round_f(n->outer.y);
    for (LlNode *p = n; p; p = p->parent) {
        x += ll_round_f(ll_value_get(&p->vx));
        y += ll_round_f(ll_value_get(&p->vy));
        if (p->kind == LL_NODE_SCROLL) {
            LlScroll *d = ll_scroll_data(p);
            if (d) {
                int po = ll_round_f(ll_scroll_pos(p));
                if (d->horizontal) {
                    x -= po;
                } else {
                    y -= po;
                }
            }
        }
    }
    return (LlRect){ (int16_t)x, (int16_t)y, ll_round_f(n->outer.w), ll_round_f(n->outer.h) };
}

/* 方向几何导航：0=上 1=下 2=左 3=右。
 *
 * 判据用"边到边的间隙"，不用中心点距离——这是几何导航最容易踩的坑。
 * 中心点法会让一整行的宽条目朝左时，命中下一行左边的那个按钮：
 * 它的中心确实偏左，可它其实在下面，根本没在"左边"。
 * 所以先算主轴上两条边之间还差多少（gap）：
 *   gap < 0 意味着两条边在主轴上是重叠/反向的，直接排除；
 *   再要求中心点也确实在这个方向上（main_off > 0），排除同一行的邻居。
 * 打分 = gap + 2*交叉轴偏移：先要最近，其次才容忍歪一点。 */
LlNode *ll_node_neighbor(LlNode *root, LlNode *from, int dir)
{
    LlRect cur;
    int    best = -1;
    float  best_score = 0.0f;
    int    horiz = (dir == 2 || dir == 3);

    if (!from) {
        return ll_node_first_focusable(root);
    }
    g_walk_n = 0;
    collect_focusable(root);
    cur = ll_node_screen_rect(from);
    if (ll_rect_empty(cur)) {
        return NULL;
    }

    for (int i = 0; i < g_walk_n; i++) {
        LlNode *c = g_walk[i];
        LlRect  r;
        float   gap, main_off, cross, score, cx1, cy1, cx2, cy2;

        if (c == from) {
            continue;
        }
        r = ll_node_screen_rect(c);
        if (ll_rect_empty(r)) {
            continue;
        }
        cx1 = cur.x + cur.w * 0.5f;
        cy1 = cur.y + cur.h * 0.5f;
        cx2 = r.x + r.w * 0.5f;
        cy2 = r.y + r.h * 0.5f;

        switch (dir) {
        case 0:   /* 上 */
            gap      = cur.y - (r.y + r.h);
            main_off = cy1 - cy2;
            cross    = ll_absf(cx2 - cx1);
            break;
        case 1:   /* 下 */
            gap      = r.y - (cur.y + cur.h);
            main_off = cy2 - cy1;
            cross    = ll_absf(cx2 - cx1);
            break;
        case 2:   /* 左 */
            gap      = cur.x - (r.x + r.w);
            main_off = cx1 - cx2;
            cross    = ll_absf(cy2 - cy1);
            break;
        default:  /* 右 */
            gap      = r.x - (cur.x + cur.w);
            main_off = cx2 - cx1;
            cross    = ll_absf(cy2 - cy1);
            break;
        }
        if (gap < 0.0f || main_off <= 0.0f) {
            continue;
        }
        (void)horiz;
        score = gap + cross * 2.0f;
        if (best < 0 || score < best_score) {
            best       = i;
            best_score = score;
        }
    }
    return best >= 0 ? g_walk[best] : NULL;
}

LlNode *ll_node_hit(LlNode *root, int16_t sx, int16_t sy)
{
    LlNode *best = NULL;
    int     x = sx, y = sy;

    if (!root) {
        return NULL;
    }
    g_walk_n = 0;
    collect_focusable(root);
    for (int i = 0; i < g_walk_n; i++) {
        LlRect r = ll_node_screen_rect(g_walk[i]);
        if (ll_rect_contains(r, x, y)) {
            best = g_walk[i];        /* 后面的更深/更新，覆盖前面的 */
        }
    }
    return best;
}

int ll_node_reveal(LlNode *n)
{
    int moved = 0;

    if (!n) {
        return 0;
    }
    for (LlNode *p = n->parent; p; p = p->parent) {
        LlScroll *d = ll_scroll_data(p);
        float     item0, item1, view_size, want, pos;

        if (!d) {
            continue;
        }
        view_size = d->view_size;
        if (d->horizontal) {
            item0 = n->outer.x;
            item1 = n->outer.x + n->outer.w;
        } else {
            item0 = n->outer.y;
            item1 = n->outer.y + n->outer.h;
        }
        pos  = ll_value_get(&d->pos);
        /* 已经完整在视口里就不动；否则把它放到正中间（两端自动夹紧） */
        if (item0 - pos >= 0.0f && item1 - pos <= view_size) {
            continue;
        }
        want = item0 - (view_size - (item1 - item0)) * 0.5f;
        d->anchor = n;
        ll_scroll_to(p, want);
        moved = 1;
    }
    return moved;
}
