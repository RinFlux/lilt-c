/* ll_cursor.h — 全局光标（移植自 lilt 的 ui/cursor.go）
 *
 * 整个界面只有一个光标：一个跟着焦点走的圆角矩形，在单色屏上靠"反色"显示。
 * 它不是一个值，而是**四条边各自一个 LlValue**——这是 lilt 手感最妙的一处：
 *   往下的时候下边用 Lead（快）、上边用 Trail（慢），光标就朝行进方向拉长；
 *   到位后两条边一起收拢，像是甩过去又落回来。
 * 另外三种状态各用一支弹簧：按住收缩用 Release（带回弹），跨页大跳用 Leap
 * （落地压扁一下），按住方向键连发时朝那个方向"探"出去用 Lean。
 */
#ifndef LL_CURSOR_H
#define LL_CURSOR_H

#include "ll_gfx.h"

/* 初始化/隐藏。hide 之后光标不参与绘制，直到第一次 move。 */
void ll_cursor_init(void);
void ll_cursor_show(int on);
int  ll_cursor_visible(void);

/* 焦点矩形的四条边；内部按移动方向挑弹簧，大跨度自动改用 Leap。 */
void ll_cursor_move(LlRect want);

/* 非 0 = 焦点正被按下：四边一起往里缩，用 Release 弹簧，松手弹回。 */
void ll_cursor_press(int down);

/* 朝某个方向探出去几像素（连发按住时用）；dx/dy 传 0 表示收回。 */
void ll_cursor_lean(int dx, int dy);

/* 当前显示中的矩形，调试与测试用。 */
LlRect ll_cursor_rect(void);

/* 画光标。必须在整棵树画完之后调用——它反色的是"已经画上去的像素"。 */
void ll_cursor_paint(LlCanvas c);

/* 光标在动吗？（静止界面可以省掉一次重画） */
int ll_cursor_busy(void);

#endif /* LL_CURSOR_H */
