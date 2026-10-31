/* Tour.ino — 自动巡演固件：上电就自己动，不用敲任何键
 *
 * 装库：整个文件夹拷进 Arduino 的 libraries 目录（IDE 里 文件 -> 首选项 ->
 *       项目位置 就是 sketchbook，它下面那层 libraries），重启 IDE，然后
 *       文件 -> 示例 -> 贡献库 -> lilt-c -> Tour 打开本文件。
 *       菜单里没有 lilt-c，先确认开发板选的是 ESP32 系列，library.properties
 *       写了 architectures=esp32，选中 Uno 时整个库连示例会被一起过滤掉。
 * 板子：开发板选 "ESP32C3 Dev Module"，端口选那个 COM；
 *       串口没输出就把 Tools -> USB CDC On Boot 打开。
 * 接线：3V3 -> VCC，GND -> GND，GPIO8 -> SDA，GPIO9 -> SCL（引脚不对改 ll_conf.h）。
 *
 * 它会自己演一圈约 20 秒：列表波浪、光标四条边、按住连发、到底橡皮筋、
 * 按下与松手的分工、整页滑入滑出、按钮反色、手感 A/B，然后回到起点重来。
 * 屏幕底部那行小字是当前这一段的编号，串口每段都会打印一句中文解释——
 * 屏上的字体只有 ASCII 那 95 个字符，没有汉字，所以中文只走串口。
 *
 * 你随时可以插手：串口里敲任意一个键（w/s/a/d、空格、j、k），巡演立刻停在
 * 当前位置，把按键全部交还给你；之后 k 键还是"手感 A/B"开关。
 *
 * 和 Gallery.ino 的一个差别值得注意：这里没有 xTaskCreate 的按键任务。
 * 事件环是"单生产者 / 单消费者"设计的（见 ll_input.c 顶部注释），
 * 巡演本身就是一个生产者，再开一个任务读串口就变成两个生产者了。
 * 所以这个例程把读按键也搬回 loop()，整条链单线程，不需要任何锁。
 * 采样率从 200Hz 降到帧率（30Hz）：消抖要 2 次采样 = 66ms，点按会变成
 * "按下 66ms 后生效、松手判定 90ms"，手感上只是稍微钝一点点。
 */
#include <Arduino.h>
#include "lilt_c.h"
#include "demo_tour.h"

/* 串口按键的消抖状态。放全局：C 里没有闭包，谁用谁持有。 */
static LlSerialKeys g_keys;
static uint32_t     g_last_ms;

/* 有人打字了就把控制权还给他（available() 只报数，不取走字节）。 */
static void take_over_if_someone_types(void)
{
    if (ll_tour_running() && Serial.available() > 0) {
        ll_tour_take_over();
    }
}

void setup(void)
{
    ll_serial_keys_init(&g_keys);
    ll_screen_init();          /* 里面会调 ll_bus_begin()：I2C + 面板寄存器 + I2C 扫描 */
    ll_tour_reset();           /* 先排好时间轴，再把第一页推上去 */

    /* 顺序有讲究：ll_pages_boot 内部先 ll_app_boot（它会 ll_anim_init 清零所有
     * 动画值），再建首页的节点树。反过来就会被清零成 0。 */
    ll_pages_boot(ll_screen_api(), &ll_demo_tour_main);
    g_last_ms = millis();
}

void loop(void)
{
    uint32_t now = millis();
    float    dt  = (float)(now - g_last_ms) / 1000.0f;

    g_last_ms = now;
    if (dt > 0.1f) {
        dt = 0.1f;             /* 和 ll_app_frame 一样：卡了一下也不要让动画跳一大步 */
    }

    take_over_if_someone_types();
    ll_serial_keys_service(&g_keys, dt);   /* 你敲的键和巡演的键进同一个事件环 */
    ll_tour_service(dt);                    /* 到点的动作塞进环里，这一帧就被 dispatch 出去 */
    ll_app_frame();                         /* 输入 -> 更新 -> 动画 -> 布局 -> 绘制 -> 推屏 */
}
