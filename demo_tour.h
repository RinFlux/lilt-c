/* demo_tour.h — 自动巡演：上电自己把整出手感演一遍，不用敲任何键
 *
 * 用法（examples/Tour/Tour.ino 里就是这么两行）：
 *   ll_tour_reset();
 *   ll_pages_boot(ll_screen_api(), &ll_demo_tour_main);
 *   loop 里每帧： ll_tour_service(dt);  ll_app_frame();
 *
 * 巡演不碰任何内部状态，它只是"按时间表往事件环里塞按键"，
 * 所以你在板子上看到的东西，和你自己敲串口敲出来的东西是同一套代码路径。
 */
#ifndef LL_DEMO_TOUR_H
#define LL_DEMO_TOUR_H

#include "ll_page.h"

/* Arduino 的 .ino 是按 C++ 编的，而这个 .c 是按 C 编的。C++ 会给函数名做
 * name mangling（ll_tour_reset -> _Z13ll_tour_resetv），两边对不上就在链接时报
 * "undefined reference"。extern "C" 是告诉 C++ 编译器"这个名字别改写"，
 * 它只影响链接期的名字，不改变任何运行行为。
 * 放在自己头里、不依赖 lilt_c.h 的包裹，是因为 #include 的先后顺序不该决定
 * 能不能链接——包一层就得管住所有层。 */
#ifdef __cplusplus
extern "C" {
#endif

extern LlPage ll_demo_tour_main;     /* 首页：7 行列表，字幕在页脚 */
extern LlPage ll_demo_tour_detail;   /* 详情页：两个按钮 + 计数，演按下/松手 */

/* 建时间轴并把游标归零。ll_pages_boot 之前调一次。 */
void ll_tour_reset(void);

/* 每帧调一次，dt = 上一帧真实耗时（秒）。和读按键放在同一个任务里。 */
void ll_tour_service(float dt);

/* 有人碰串口了：停掉巡演，把按键全部交还给用户。 */
void ll_tour_take_over(void);
int  ll_tour_running(void);

/* 录帧/自检用的五个只读查询。 */
int         ll_tour_total_ms(void);              /* 一整圈多长 */
int         ll_tour_step_count(void);            /* 时间轴排了多少步 */
int         ll_tour_truncated(void);             /* 非 0 = STEP_MAX 不够用，尾巴被切掉了 */
int         ll_tour_caption_count(void);         /* 字幕条数 */
const char *ll_tour_caption(int i);              /* 第 i 条字幕（屏上那条英文） */

#ifdef __cplusplus
}
#endif

#endif /* LL_DEMO_TOUR_H */
