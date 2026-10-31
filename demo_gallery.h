/* demo_gallery.h — B 阶段验收应用：三页小程序（列表波浪 / 详情按钮 / 开关）
 *
 * 这块屏上跑的就是最终固件用的同一套引擎；PC 无头录图和板子上完全一致。
 * 想直接看手感，把 ll_demo_gallery_main 交给 ll_pages_boot() 就行。
 */
#ifndef LL_DEMO_GALLERY_H
#define LL_DEMO_GALLERY_H

#include "ll_page.h"

/* 见 demo_tour.h 里那段注释：.ino 是 C++、.c 是 C，声明必须用 extern "C"
 * 锁住链接名，否则链接期报 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

extern LlPage ll_demo_gallery_main;      /* 首页：可滚动的列表 + 波浪 */
extern LlPage ll_demo_gallery_detail;    /* 详情页：按钮 + 计数（走 ll_app_do 队列） */
extern LlPage ll_demo_gallery_options;   /* 开关页：三个有真实效果的复选框 */

/* MENU 键循环手感档位；这里也可以一键复位。 */
void ll_gallery_reset_feel(void);

#ifdef __cplusplus
}
#endif

#endif /* LL_DEMO_GALLERY_H */
