/* Gallery.ino — B 阶段验收固件：三页小应用（列表波浪 / 详情按钮 / 开关）
 *
 * 和 SpringLab 的区别只在 setup() 的最后两行：这里交给引擎的不是一个
 * 手写页面，而是一个"页面栈"——ll_pages_boot() 会把首页推上去，之后
 * 方向键走焦、OK 点击、Back 出栈、整页滑入滑出、光标追焦点，全是框架的事。
 * 应用只写"这一页上有哪些控件"（见 demo_gallery.c 里的 build 函数）。
 *
 * 装库：整个文件夹拷进 Arduino 的 libraries 目录（IDE 里 文件 -> 首选项 ->
 *       项目位置 就是 sketchbook，它下面那层 libraries），重启 IDE，然后
 *       文件 -> 示例 -> 贡献库 -> lilt-c -> Gallery 打开本文件。
 *       菜单里没有 lilt-c，先确认开发板选的是 ESP32 系列。library.properties
 *       写了 architectures=esp32，选中 Uno 时 IDE 会把整个库连示例一起过滤掉。
 * 板子：开发板选 "ESP32C3 Dev Module"，端口选那个 COM；
 *       串口没输出就把 Tools -> USB CDC On Boot 打开。
 *
 * 能玩到什么：
 *   w/s/a/d   上下左右走焦（列表滚到底再按会顶出橡皮筋，松手弹回）
 *   空格      按下=OK（松手才真正触发，按住不放会连发）
 *   j         Back，退一页
 *   k         Menu，在首页循环换"手感档位"（Default/Smooth/Snappy/Bouncy）
 *   开关页里 invert 是真的把前景背景互换，wide rows 会改到首页的行高
 */
#include <Arduino.h>
#include "lilt_c.h"
#include "demo_gallery.h"

/* 串口按键的消抖状态。放全局：C 里没有闭包，谁用谁持有。 */
static LlSerialKeys g_keys;

/* 按键任务：每 5ms 采一次样，和帧循环分开跑。
 * 单生产者（这里）/ 单消费者（帧循环）的事件环不需要锁，见 ll_input.c。 */
static void key_task(void *arg)
{
    const int   tick_ms = 1000 / LL_KEY_POLL_HZ;
    const float dt      = 1.0f / LL_KEY_POLL_HZ;

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
    ll_gallery_reset_feel();          /* 手感档位回到 Default(0.29/0.75) */

    /* 顺序有讲究：ll_pages_boot 内部先 ll_app_boot（它会 ll_anim_init 清零所有
     * 动画值），再建首页的节点树。反过来就会被清零成 0。 */
    ll_pages_boot(ll_screen_api(), &ll_demo_gallery_main);

    /* 栈 4096 字节（arduino-esp32 的 xTaskCreate 按字节算，不是 FreeRTOS 的字数） */
    xTaskCreate(key_task, "keys", 4096, NULL, 1, NULL);
}

void loop(void)
{
    ll_app_frame();                   /* 跑一帧，内部自己限速到 ll_app_fps */
}
