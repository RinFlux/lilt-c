/* ll_screen.h — 1 位帧缓冲实现的 LlScreen（SSD1306/SH1106 的页式内存布局）
 *
 * 一个字节竖着管 8 个像素，低位在上——这正好是面板显存的格式，所以推一帧
 * 就是整块拷到总线上，不需要逐像素转换。lilt 的 driver/sh1106/draw.go 就是这么
 * 做的，这里的按页位掩码填充/反色算法与它一致。
 *
 * 反色是在缓冲里 XOR，所以光标可以做真正的"亮暗对调"（iPod 菜单那种），
 * 不必依赖面板的整屏反色命令。
 */
#ifndef LL_MONO_H
#define LL_MONO_H

#include <stdint.h>
#include "ll_conf.h"
#include "ll_gfx.h"

#define LL_SCREEN_PAGES (LL_SCREEN_H / 8)

typedef struct {
    uint8_t  buf[LL_SCREEN_W * LL_SCREEN_PAGES];
    uint8_t  dirty;              /* 每页一个 bit，置位表示这帧要重推 */
    LlScreen api;                /* 交给 Canvas 的接口 */
} LlMonoScreen;

extern LlMonoScreen ll_mono;

/* 复位缓冲并初始化总线。 */
void ll_screen_init(void);

/* 返回可直接交给 Canvas 的接口指针。 */
LlScreen *ll_screen_api(void);

/* 整屏刷成一种颜色（每帧开头 lilt 就用主题背景色这么干）。 */
void ll_screen_clear(LlColor c);

/* 标脏：y 行起、h 行涉及的页要重推。 */
void ll_screen_mark_dirty(int16_t y, int16_t h);

/* 把脏页推给面板，然后清脏位。没有脏页就直接返回，静止画面不占总线。 */
int ll_screen_flush(void);

/* 诊断用：当前脏页掩码。 */
uint8_t ll_screen_dirty_mask(void);

#endif /* LL_MONO_H */
