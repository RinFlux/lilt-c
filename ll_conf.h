/* ll_conf.h — 整个移植工程的可调参数（一份文件管全部硬件差异）
 *
 * 改这里的值就能适配不同的板子/屏幕，其他 .c 里不出现魔法数字。
 */
#ifndef LL_CONF_H
#define LL_CONF_H

/* ---------- 屏幕 ---------- */
/* 0.96 寸 I2C OLED：SSD1306 是 128x64；有些"看起来一样"的模块其实是 SH1106，
 * 它的显存宽 132 列，需要 2 列偏移。如果你的屏右边缺 2 列或整体偏了，
 * 把 LL_PANEL_IS_SH1106 置 1 试试。 */
#define LL_SCREEN_W              128
#define LL_SCREEN_H              64
#define LL_PANEL_IS_SH1106       0     /* 1 = 按 SH1106 时序推页（列偏移 2） */
#define LL_I2C_ADDR              0x3C  /* 常见 0x3C，少数模块是 0x3D */
#define LL_ROTATE_180            0     /* 屏幕装反了就把 6 行接线翻过来 */

/* I2C 引脚：arduino-esp32 给 ESP32-C3 的默认就是 SDA=8 / SCL=9，
 * ESP32-C3 SuperMini 引出的也正是这两个。如果你的板子标了别的脚，改这里。 */
#define LL_I2C_SDA               8
#define LL_I2C_SCL               9

/* ---------- 帧率 ----------
 * I2C 是这里的瓶颈：128x64 单色一帧要推 1024 字节，@400kHz 约 23ms。
 * 弹簧动画按 dt 积分，帧率低不会改变手感，只会让每帧之间的间隔变大。 */
#define LL_MAX_FPS               30
#define LL_KEY_POLL_HZ           200   /* 消抖按"连续采样次数"计，周期 5ms×2≈10ms */

/* ---------- 输入 ----------
 * 现在按键来自串口（'w a s d' 方向、空格/回车 = OK、'j' = Back、'k' = Menu）。
 * 接上真按键后，把 ll_input_boot() 里的串口读取换成对每个引脚调用
 * ll_keypoll_poll(..., digitalRead(pin))，其余代码一行都不用改。 */
#define LL_SERIAL_BAUD           115200

/* ---------- 静态池容量（RAM 占用，按你的实际界面调） ---------- */
#define LL_ANIM_MAX_VALUES       96    /* 同时在动的弹簧上限，每个 28 字节 */
#define LL_INPUT_QUEUE           32    /* 与 lilt 的事件队列容量一致 */
#define LL_NODE_MAX              96    /* 节点池 */
#define LL_NODE_MAX_KIDS         16    /* 一个容器最多直接装几个孩子 */
#define LL_SCROLL_MAX            6     /* 同时存在的滚动容器数（一屏一般 1~2 个） */
#define LL_PAGE_MAX              4     /* 页面栈深度 */
#define LL_UI_DO_QUEUE           8     /* 两帧之间执行的改动队列 */
#define LL_JOB_MAX               4     /* 后台回调并发数 */

#endif /* LL_CONF_H */
