/* demo_spring.h — A 阶段验收页：四个弹簧实验区
 *
 * 这块屏上跑的就是最终固件用的同一套引擎，PC 无头录图和板子上完全一致。
 * 目的只有一个：让你能用眼睛核对 lilt 的手感有没有被忠实搬过来。
 */
#ifndef LL_DEMO_SPRING_H
#define LL_DEMO_SPRING_H

#include "ll_app.h"

/* 见 demo_tour.h 里那段注释：.ino 是 C++、.c 是 C，声明必须用 extern "C"
 * 锁住链接名，否则链接期报 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

extern LlScene ll_demo_spring;

/* 回到初始状态（Back 键也会调它）。 */
void ll_demo_spring_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* LL_DEMO_SPRING_H */
