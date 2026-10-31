/* ll_page.h — 页面栈与默认按键路由（移植自 lilt 的 ui/app.go + ui/scroll.go 的焦点逻辑）
 *
 * 一页 = 一棵节点树 + 一个滚动视口。框架负责：
 *   建根（Column -> Scroll -> 内容 Column）、进出场动画、焦点与光标、
 *   方向键走焦、OK 触发点击、Back 出栈、到底之后的橡皮筋。
 * 应用只写 build()（往上挂控件）和可选的 key()（比如 MENU 换主题）。
 *
 * 节点回收按"页号"整批做：建树前把 gen 设成栈深，出栈时 ll_node_free_from(gen)
 * 一次回收这一页的全部节点，不用逐个记谁是谁的孩子。
 */
#ifndef LL_PAGE_H
#define LL_PAGE_H

#include "ll_app.h"
#include "ll_input.h"
#include "ll_node.h"

typedef struct LlPage LlPage;

struct LlPage {
    const char *name;
    void (*build)(LlPage *self);                  /* 往 self->content 上挂控件 */
    int  (*key)(LlPage *self, const LlKeyEvent *ev);  /* 返回 1 = 我处理了 */
    void (*enter)(LlPage *self);
    void (*leave)(LlPage *self);
    void *user;

    /* 以下由框架填写 */
    LlNode *root;        /* 竖排 Column，整页的根 */
    LlNode *scroller;    /* 滚动容器：波浪、橡皮筋都在这 */
    LlNode *content;     /* build 的孩子挂这里 */
    LlNode *focus;       /* 这页记住的焦点，回来看它还在不在 */
    int     gen;         /* 页号 = 入栈时的深度 */
    int     exiting;     /* 正在滑出，动画走完再回收 */
};

/* 上电一次：清池、清光标、把第一页推上去。 */
void     ll_pages_boot(LlScreen *screen, LlPage *first);
void     ll_page_push(LlPage *p);
void     ll_page_pop(void);
void     ll_page_replace(LlPage *p);              /* 换掉当前页（不留历史） */
LlPage  *ll_page_top(void);
int      ll_page_depth(void);

/* build 里用：往内容列上加一个控件。 */
void     ll_page_put(LlPage *p, LlNode *n);
/* 直接把某个控件的滚动内容列换成 n（用于详情页复用同一页壳子）。 */
void     ll_page_set_content(LlPage *p, LlNode *n);
/* 移动焦点并把它滚进视口。 */
void     ll_page_focus(LlNode *n);
LlNode  *ll_page_focused(void);
/* 焦点所在（或最近）的滚动容器，没有则 NULL。 */
LlNode  *ll_page_focus_scroller(void);

/* 默认按键路由。scene 用它，主机测试也可以直接喂事件。 */
int      ll_page_handle_key(const LlKeyEvent *ev);

/* 作为 LlScene 交给 ll_app_boot。 */
extern LlScene ll_page_scene;

#endif /* LL_PAGE_H */
