/* ll_node.h — 节点树与盒模型（移植自 lilt 的 ui/box.go、ui/node.go、ui/flex.go）
 *
 * Go 版里每个控件是实现 ui.Node 接口的 struct；C 里没有接口，这里反过来：
 * 一个 LlNode 结构 + kind 字段 + 每种 kind 的私有数据（滚动容器用小池）。
 * 好处是树里所有节点同构，遍历/裁剪/焦点/动画都能用同一套代码；
 * 代价是新增控件要在 switch 里加一支——对这块屏的规模来说划得来。
 *
 * 三条和手感直接相关的规则，都照抄 lilt：
 *   1) 布局用 float 算，下笔才取整，所以弹簧驱动的位移是平滑的；
 *   2) 尺寸/位置变化时不"瞬移"：节点带一对 LlValue，Shift 一下让它滑过去；
 *   3) 一切以"单位"计（ll_unit() 像素），换字体时整套间距一起放大。
 *
 * 和 Go 版的一处有意为之的差别：布局结果 outer 存的是**屏幕绝对坐标**。
 * lilt 用相对坐标 + 变换栈，因为它要支持窗口和任意嵌套平移；我们只有一块屏，
 * 绝对坐标让光标定位、命中测试、几何导航都能直接读矩形，不必回溯整条祖先链。
 * 动画位移仍然沿祖先链叠加（见 ll_node_screen_rect 与绘制里的 translate）。
 */
#ifndef LL_NODE_H
#define LL_NODE_H

#include "ll_anim.h"
#include "ll_geom.h"
#include "ll_gfx.h"
#include "ll_theme.h"

typedef struct LlNode LlNode;

typedef enum {
    LL_NODE_BOX = 0,    /* 容器：Column / Row */
    LL_NODE_LABEL,      /* 一行文字，超出宽度裁剪 */
    LL_NODE_BUTTON,     /* 文字 + 边框 + 点击 */
    LL_NODE_LIST_ITEM,  /* 列表里的一行：文字 + 右侧箭头 + 点击 */
    LL_NODE_CHECKBOX,   /* 方框 + 文字 + 勾选 */
    LL_NODE_SCROLL,     /* 视口 + 一块比视口大的内容，滚动和波浪都在这 */
    LL_NODE_KIND_COUNT
} LlNodeKind;

typedef enum { LL_DIR_NONE = 0, LL_DIR_COLUMN, LL_DIR_ROW } LlDir;

/* 节点标志位。 */
#define LL_F_VISIBLE   0x01   /* 参与布局和绘制 */
#define LL_F_FOCUSABLE 0x02   /* 能被光标停上（按钮、列表行、复选框） */
#define LL_F_PRESSED   0x04   /* 正被按着：光标收缩 */
#define LL_F_ANIM_POS  0x08   /* 位置变化时滑过去，而不是跳过去 */
#define LL_F_LIKED     0x10   /* 复选框的勾 / 选中态 */
#define LL_F_DIRTY     0x20   /* 属性变了，下一帧要重排 */

struct LlNode {
    LlNodeKind   kind;
    LlDir        dir;            /* 容器主轴方向 */
    LlNode      *parent;
    LlNode      *kids[LL_NODE_MAX_KIDS];
    uint8_t      n_kids;
    uint8_t      flags;
    uint8_t      gen;            /* 属于第几页，出栈时按它整批回收 */

    LlInsets     pad;            /* 已经是像素（构造时乘过 ll_unit()） */
    LlInsets     mar;
    float        grow;           /* 0 = 占内容大小；>0 = 按比例抢剩余 */
    float        gap_px;         /* 容器孩子间距，像素 */
    int16_t      fix_w, fix_h;   /* >0 就锁死这一轴（不含 margin） */
    int          main_align;     /* 主轴对齐：0 起始 1 居中 2 末尾 3 两端 */
    int          cross_align;    /* 交叉轴对齐：同上，3 表示拉伸 */

    LlRectF      box;            /* 布局输入：含自己 margin 的绝对矩形 */
    LlRectF      outer;          /* 去掉 margin 后的绝对矩形（光标和命中都用它） */
    LlValue      vx, vy;         /* 位置动画：画的时候叠加这两个值 */
    int16_t      pref_w, pref_h; /* measure 出来的首选占用（含 pad + mar） */

    const char  *text;           /* 文案；必须是活得比节点久的字符串 */
    const LlFont *font;          /* NULL = 用主题字体 */
    void        *user;           /* LL_NODE_SCROLL 指向 LlScroll */
};

/* ---- 滚动容器的私有数据：一小池，不 malloc ---- */
typedef struct {
    LlNode   *content;          /* 比视口大的内容 */
    LlNode   *anchor;           /* 波浪中心（一般是当前焦点行） */
    LlValue   pos;              /* 视口主轴偏移，像素 */
    LlValue   pull;             /* 超滚的原始位移，显示时过橡皮筋 */
    float     view_size;        /* 视口主轴长度 */
    float     content_size;     /* 内容主轴长度 */
    LlRectF   viewport;         /* 裁剪用的绝对矩形 */
    uint8_t   horizontal;       /* 1 = 横向 */
} LlScroll;

/* ---- 池：不 malloc，用完回收 ---- */
void    ll_nodes_init(void);
LlNode *ll_node_new(LlNodeKind kind);           /* 从池里要一个，清零 */
void    ll_node_free_from(int gen);             /* 回收 gen >= 这个页的所有节点 */
int     ll_node_gen(void);                      /* 当前页号，建树时打戳用 */
void    ll_node_set_gen(int gen);
int     ll_nodes_used(void);

/* ---- 建树 ---- */
void ll_add(LlNode *parent, LlNode *child);
void ll_set_padding_u(LlNode *n, LlInsets units);  /* 单位版：内部乘 ll_unit() */
void ll_set_margin_u(LlNode *n, LlInsets units);
void ll_set_gap_u(LlNode *n, float units);
void ll_set_fixed_size(LlNode *n, int16_t w, int16_t h);
void ll_set_grow(LlNode *n, float g);
void ll_set_flags(LlNode *n, uint8_t on, uint8_t off);
void ll_animate_layout(LlNode *n);               /* 位置变化滑过去 */
void ll_node_set_text(LlNode *n, const char *text);
void ll_node_set_liked(LlNode *n, int liked);
int  ll_node_liked(const LlNode *n);

/* ---- 便捷构造（对应 widget.NewLabel / NewButton / NewListItem / NewScroll） ---- */
LlNode *ll_label(const char *text);
LlNode *ll_label_small(const char *text);
typedef void (*LlClickFn)(void *user);
LlNode *ll_button(const char *text, LlClickFn on_click, void *user);
LlNode *ll_list_item(const char *text, LlClickFn on_click, void *user);
LlNode *ll_check_box(const char *text, int checked, LlClickFn on_toggle, void *user);
LlNode *ll_column(void);
LlNode *ll_row(void);
LlNode *ll_list(void);            /* = 一个竖排的 Column，行距用主题 Gap */
LlNode *ll_scroll(LlNode *content, int horizontal);

LlClickFn ll_node_click_of(const LlNode *n);
void     *ll_node_user_of(const LlNode *n);
LlScroll *ll_scroll_data(const LlNode *n);       /* 不是滚动容器就返回 NULL */

/* ---- 布局与绘制 ---- */
/* 自底向上量：算出每个节点的 pref_w/pref_h（含边距）。 */
void ll_node_measure(LlNode *n);
/* 自顶向下摆：box 是父分配给它的绝对矩形（含它自己的 margin）。 */
void ll_node_layout(LlNode *n, LlRectF box);
/* 递归画：内部负责平移、裁剪、位置动画。 */
void ll_node_paint(LlNode *n, LlCanvas canvas);

/* 一整棵树走一帧：measure + layout 到给定尺寸（通常就是屏幕）。 */
void ll_layout_tree(LlNode *root, int16_t w, int16_t h);

/* 把节点滚进最近祖先视口的中心偏好位置；返回是否动了。 */
int ll_node_reveal(LlNode *n);

/* ---- 滚动 ---- */
/* 跳到 pos（0..max）；做了 lilt 的"内容跳、行错峰滑回"，波浪就是这么来的。 */
void ll_scroll_to(LlNode *n, float pos);
void ll_scroll_shift(LlNode *n, float d);        /* 相对当前位置挪一点 */
float ll_scroll_pos(const LlNode *n);            /* 含橡皮筋的显示位置 */
float ll_scroll_max(const LlNode *n);
void ll_scroll_overscroll(LlNode *n, float d);   /* 到底了再推一把：松手弹回 */
void ll_scroll_set_anchor(LlNode *n, LlNode *anchor);

/* ---- 遍历与命中 ---- */
LlNode *ll_node_first_focusable(LlNode *root);
LlNode *ll_node_next_focusable(LlNode *root, LlNode *from);   /* 树序下一个 */
LlNode *ll_node_prev_focusable(LlNode *root, LlNode *from);
/* 沿主轴找最近的可聚焦兄弟：方向是 0=上 1=下 2=左 3=右。
 * 判据是"边到边的间隙"加交叉轴偏移，整行的条目朝左/朝右不会串到下一行去。 */
LlNode *ll_node_neighbor(LlNode *root, LlNode *from, int dir);
/* 节点当前真正显示在屏幕上的矩形（把各级动画和滚动偏移都算上）。 */
LlRect ll_node_screen_rect(LlNode *n);
/* 屏幕坐标命中：返回最深那个可聚焦节点。 */
LlNode *ll_node_hit(LlNode *root, int16_t x, int16_t y);

/* 焦点（全局一个，和 lilt 一样）。 */
extern LlNode *ll_focus;

#endif /* LL_NODE_H */
