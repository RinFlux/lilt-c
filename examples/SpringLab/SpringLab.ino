/* SpringLab.ino — A 阶段验收固件：把弹簧实验页跑在 ESP32-C3 + 0.96 OLED 上
 *
 * 这个 .ino 只做三件事：初始化、把按键任务起来、然后一遍遍跑帧循环。
 * 引擎（弹簧 / 帧缓冲 / 光标 / 布局）全在 lilt-c 库里，和 PC 上录帧用的是
 * 同一份 .c，一个字节都没改——唯一的差别是 ll_bus 接缝换成了这块板子的实现
 * （库根目录的 ll_board.cpp，两个示例共用它）。
 *
 * 装库：整个文件夹拷进 Arduino 的 libraries 目录（IDE 里 文件 -> 首选项 ->
 *       项目位置 就是 sketchbook，它下面那层 libraries），重启 IDE，然后
 *       文件 -> 示例 -> 贡献库 -> lilt-c -> SpringLab 打开本文件。
 *       菜单里没有 lilt-c，先确认开发板选的是 ESP32 系列，library.properties
 *       写了 architectures=esp32，选中 Uno 时整个库连示例会被一起过滤掉。
 * 板子：开发板选 "ESP32C3 Dev Module"，端口选那个 COM；
 *       如果串口没输出，把 Tools -> USB CDC On Boot 打开。
 */
#include <Arduino.h>
#include "lilt_c.h"
#include "demo_spring.h"

/* 串口按键的消抖状态。放全局：C 里没有闭包，谁用谁持有。 */
static LlSerialKeys g_keys;

/* 按键任务：每 5ms 采一次样。
 *
 * 为什么不放在 loop() 里？因为 loop() 大部分时间正忙着 delay 到下一帧
 * （30fps 一帧 33ms），拿不到 5ms 的采样节拍；消抖是按"连续采样次数"算的，
 * 节拍不准手感就变。FreeRTOS 的 delay() 会让出 CPU，所以这个任务能在
 * 帧循环睡觉的时候照常跑——这就是 Go 版里那个 goroutine 的 C 版本。
 *
 * 单生产者（这里）/ 单消费者（帧循环）的事件环不需要锁，见 ll_input.c。 */
static void key_task(void *arg)
{
    const int     tick_ms = 1000 / LL_KEY_POLL_HZ;
    const float   dt      = 1.0f / LL_KEY_POLL_HZ;

    (void)arg;
    for (;;) {
        ll_serial_keys_service(&g_keys, dt);
        vTaskDelay(pdMS_TO_TICKS(tick_ms));
    }
}

void setup(void)
{
    ll_serial_keys_init(&g_keys);
    ll_screen_init();                 /* 里面会调 ll_bus_begin()：I2C + 面板寄存器 */
    ll_demo_spring_reset();
    ll_app_boot(ll_screen_api(), &ll_demo_spring);

    /* 栈 4096 字节（arduino-esp32 的 xTaskCreate 按字节算，不是 FreeRTOS 的字数） */
    xTaskCreate(key_task, "keys", 4096, NULL, 1, NULL);
}

void loop(void)
{
    ll_app_frame();                   /* 跑一帧，内部自己限速到 ll_app_fps */
}
