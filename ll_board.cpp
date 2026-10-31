/* ll_board.cpp — ESP32-C3 + 0.96 寸 I2C OLED：ll_bus.h 的"板子端"实现
 *
 * 整个引擎只有这一个文件碰硬件。它做的事情和 Go 版里
 * driver/sh1106 + examples/esp32s3-generic 的 main 加起来一样：
 *   1) 初始化 I2C 和面板寄存器；
 *   2) 把帧缓冲里"这一帧真正变了的那几页"推出去（静止时一字节都不发）；
 *   3) 提供毫秒时钟、休息和串口字节。
 *
 * 接线（ESP32-C3 SuperMini ↔ 0.96 OLED，4 根线）：
 *   3V3  -> VCC      GND -> GND      GPIO8 -> SDA      GPIO9 -> SCL
 * 引脚不对就在 ll_conf.h 里改 LL_I2C_SDA / LL_I2C_SCL。
 *
 * 串口按键：USB 口上 115200，发小写字母 = 按下，大写字母 = 松开
 *   w/s/a/d = 上下左右，空格 = OK，x = 松开 OK，j = Back，k = Menu，h = 退格
 * 如果串口没输出：ESP32-C3 要用原生 USB 口，得在 IDE 的 Tools 里把
 * "USB CDC On Boot" 打开（用外接 UART 芯片的板子则保持默认）。
 */
#include <Arduino.h>
#include <Wire.h>

extern "C" {
#include "ll_bus.h"
#include "ll_conf.h"
#include "ll_screen.h"     /* LL_SCREEN_PAGES */
}

/* ---------------- 小工具 ---------------- */

/* I2C 每个事务的第一个字节是控制字节：bit7=Co(继续)，bit6..5=0，bit0=D/C#
 * （0=命令，1=数据）。所以命令和数据必须分两次事务发。 */
#define CTRL_CMD   0x00
#define CTRL_DATA  0x40

static void oled_cmd(uint8_t c)
{
    Wire.beginTransmission(LL_I2C_ADDR);
    Wire.write(CTRL_CMD);
    Wire.write(c);
    Wire.endTransmission();
}

static void oled_cmd2(uint8_t c, uint8_t d)
{
    oled_cmd(c);
    oled_cmd(d);
}

#if LL_ROTATE_180
/* 半字节倒序表：整字节倒序 = 高低半字节交换后各自倒序。16 字节的表，不占地方。 */
static const uint8_t NIBBLE_REV[16] = {
    0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
    0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF
};

static uint8_t rev8(uint8_t b)
{
    return (uint8_t)((NIBBLE_REV[b & 0x0F] << 4) | NIBBLE_REV[b >> 4]);
}
#endif

/* 上电扫一遍总线，把看到的地址打出来——屏不亮时第一件事就是看这行。 */
static int i2c_scan(void)
{
    int found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  I2C 设备 @ 0x%02X\n", addr);
            found++;
        }
    }
    return found;
}

static void panel_init(void)
{
    oled_cmd(0xAE);                       /* 显示关 */
    oled_cmd2(0xD5, 0x80);                /* 时钟分频：默认 */
#if LL_PANEL_IS_SH1106
    oled_cmd2(0xA8, 0x3F);                /* 多路复用 64 */
    oled_cmd2(0xD3, 0x00);                /* 显示偏移 0 */
    oled_cmd(0x40);                       /* 起始行 0 */
    oled_cmd2(0xAD, 0x8B);                /* SH1106 的 DC-DC 开 */
    oled_cmd2(0xD9, 0x1F);                /* 预充 */
    oled_cmd2(0xDB, 0x40);                /* VCOMH */
    oled_cmd2(0x30, 0x00);                /* 泵表 */
#else
    oled_cmd2(0xA8, 0x3F);                /* 多路复用 64 */
    oled_cmd2(0xD3, 0x00);                /* 显示偏移 0 */
    oled_cmd(0x40);                       /* 起始行 0 */
    oled_cmd2(0x8D, 0x14);                /* 电荷泵开：0x10 = 不用泵（少数模块） */
    oled_cmd2(0xD9, 0xF1);                /* 预充 */
    oled_cmd2(0xDB, 0x40);                /* VCOMH */
#endif
    oled_cmd(0xA0 + (LL_ROTATE_180 ? 1 : 0));   /* 段重映射（左右翻） */
    oled_cmd(0xC0 + (LL_ROTATE_180 ? 0x08 : 0));/* COM 扫描（上下翻） */
    oled_cmd2(0xDA, 0x12);                /* COM 引脚配置：128x64 用 0x12 */
    oled_cmd2(0x81, 0xCF);                /* 对比度：0..255，觉得暗就往上加 */
    oled_cmd(0xA4);                       /* 显示跟随 RAM */
    oled_cmd(0xA6);                       /* 正常（非反相） */
    oled_cmd2(0x20, 0x02);                /* 寻址模式：页寻址，和 ll_bus_push_page 对得上 */
    oled_cmd(0xAF);                       /* 显示开 */
}

/* ---------------- ll_bus 的六个实现 ---------------- */

extern "C" int ll_bus_begin(void)
{
    Serial.begin(LL_SERIAL_BAUD);
    Serial.println();
    Serial.println("lilt-c / ESP32-C3");

    Wire.begin(LL_I2C_SDA, LL_I2C_SCL, 400000);   /* 400kHz：一帧 1KB 约 23ms */

    int found = i2c_scan();
    if (found == 0) {
        Serial.println("总线上什么都没有：查 VCC/GND/SDA/SCL 和 ll_conf.h 里的引脚");
        return 1;
    }
    panel_init();
    Serial.printf("面板已初始化，%dx%d，%s，地址 0x%02X\n", LL_SCREEN_W, LL_SCREEN_H,
                  LL_PANEL_IS_SH1106 ? "SH1106" : "SSD1306", LL_I2C_ADDR);
    return 0;
}

extern "C" int ll_bus_push_page(int16_t page, const uint8_t *row, int16_t len)
{
    uint8_t col0 = LL_PANEL_IS_SH1106 ? 2 : 0;    /* SH1106 显存 132 列，画面居中偏 2 */

    if (page < 0 || page >= LL_SCREEN_PAGES || row == NULL) {
        return 1;
    }
    if (len > LL_SCREEN_W) {
        len = LL_SCREEN_W;
    }
#if LL_ROTATE_180
    /* 装反了：页序倒过来、行内字节序倒过来、每个字节再整体倒序。
     * 这样 ll_screen.c 完全不用知道自己被转了 180 度。 */
    {
        uint8_t tmp[LL_SCREEN_W];
        int16_t p = LL_SCREEN_PAGES - 1 - page;

        for (int16_t i = 0; i < len; i++) {
            tmp[i] = rev8(row[len - 1 - i]);
        }
        row = tmp;
        page = p;
    }
#endif
    oled_cmd((uint8_t)(0xB0 | page));
    oled_cmd((uint8_t)(0x00 | (col0 & 0x0F)));
    oled_cmd((uint8_t)(0x10 | (col0 >> 4)));
    oled_cmd(0x40);                              /* 起始行回 0 */

    Wire.beginTransmission(LL_I2C_ADDR);
    Wire.write(CTRL_DATA);
    for (int16_t i = 0; i < len; i++) {
        Wire.write(row[i]);
    }
    return (int)Wire.endTransmission();
}

extern "C" int ll_bus_flush(void)
{
    return 0;    /* 每页推出去就已经在屏上了，这里没有要收尾的东西 */
}

extern "C" uint32_t ll_bus_millis(void)
{
    return millis();
}

extern "C" void ll_bus_delay(int ms)
{
    if (ms > 0) {
        delay((uint32_t)ms);    /* delay() 会让出 CPU，按键任务才有机会跑 */
    }
}

extern "C" int ll_bus_read_key_byte(void)
{
    if (Serial.available() > 0) {
        return Serial.read();
    }
    return -1;
}

extern "C" void ll_bus_log(const char *line)
{
    Serial.println(line);
}
