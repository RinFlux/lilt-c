/* ll_bus.h — 唯一的硬件接缝
 *
 * 帧缓冲、裁剪、动画这些都在纯 C 里算，只有"把一页数据推给屏"和"取当前时间"
 * 需要真正的板子。Arduino 侧在 ll_board.cpp 里用 Wire 实现（extern "C"），
 * 主机侧在 host/host_bus.c 里实现成输出 PPM 图片——同一份引擎代码两边都能跑。
 */
#ifndef LL_BUS_H
#define LL_BUS_H

#include <stdint.h>

/* 上电、初始化面板（SSD1306/SH1106 的寄存器序列）。返回 0 成功。 */
int ll_bus_begin(void);

/* 推送一帧缓冲的一页：page 是 0..(H/8-1)，row 是该页的 LL_SCREEN_W 个字节
 * （每字节竖着管 8 个像素，低位在上）。只推变化的页，是 I2C 上保住帧率的关键。 */
int ll_bus_push_page(int16_t page, const uint8_t *row, int16_t len);

/* 一帧推完的收尾。设备端是空操作，主机端在这里把缓存写成 PPM 图片。 */
int ll_bus_flush(void);

/* 毫秒计数，用于帧循环的 dt。 */
uint32_t ll_bus_millis(void);

/* 让出 CPU 休息 ms 毫秒（设备端 delay，主机端不睡以便快点录图）。 */
void ll_bus_delay(int ms);

/* 串口：读一个字节，没有则返回 -1。按键现在从这里来。 */
int ll_bus_read_key_byte(void);

/* 打印一行调试信息（host 端就是 printf）。 */
void ll_bus_log(const char *line);

#endif /* LL_BUS_H */
