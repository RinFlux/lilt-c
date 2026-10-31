/* host_bus.c — 主机侧的"硬件"：把帧缓冲写成文件，时间由脚本推进
 *
 * 这套 harness 和板子上的固件跑的是同一份引擎代码（ll_*.c 一个字节都不改），
 * 只是把唯一的接缝 ll_bus 换成 PC 实现。这样我能在你烧板子之前先看到画面。
 */
#ifdef ARDUINO
/* Arduino 会把库目录下所有 .c 一起收进编译。这个文件是 PC 侧的无头录帧 harness（自己实现了
 * ll_bus_*，还带一个 main()），在板子上必须整个编成空翻译单元，否则会和库根目录的
 * ll_board.cpp 撞符号、再多一个 main。它们只在 host/build.sh 那条链路里参与编译。 */
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ll_bus.h"
#include "../ll_conf.h"
#include "../ll_screen.h"

#define PANEL_BYTES (LL_SCREEN_W * LL_SCREEN_PAGES)

static uint8_t  g_panel[PANEL_BYTES];
static FILE    *g_raw;
static uint32_t g_ms;
static int      g_frames;
static char     g_path[64] = "out/frames.raw";

/* 换输出文件：B 阶段的录帧要和 A 阶段分开存，不然两套没法同时看。 */
void host_set_output(const char *path)
{
    snprintf(g_path, sizeof(g_path), "%s", path);
}

int ll_bus_begin(void)
{
    memset(g_panel, 0, sizeof(g_panel));
    g_ms = 0;
    g_frames = 0;
    if (g_raw) {
        fclose(g_raw);
    }
    g_raw = fopen(g_path, "wb");
    if (!g_raw) {
        fprintf(stderr, "无法创建 %s（请先 mkdir out）\n", g_path);
        return 1;
    }
    return 0;
}

int ll_bus_push_page(int16_t page, const uint8_t *row, int16_t len)
{
    if (page < 0 || page >= LL_SCREEN_PAGES) {
        return 1;
    }
    if (len > LL_SCREEN_W) {
        len = LL_SCREEN_W;
    }
    memcpy(&g_panel[page * LL_SCREEN_W], row, (size_t)len);
    return 0;
}

int ll_bus_flush(void) { return 0; }

uint32_t ll_bus_millis(void) { return g_ms; }

void ll_bus_delay(int ms)
{
    if (ms < 0) ms = 0;
    g_ms += (uint32_t)ms;      /* 主机上不真的睡，只是让虚拟时钟前进 */
}

int ll_bus_read_key_byte(void) { return -1; }   /* 脚本直接喂事件，不用串口 */

void ll_bus_log(const char *line) { fprintf(stderr, "%s\n", line); }

/* ---- harness 自己用的接口 ---- */
void host_advance_ms(uint32_t ms) { g_ms += ms; }

int host_snapshot(void)
{
    if (!g_raw) {
        return 1;
    }
    fwrite(g_panel, 1, PANEL_BYTES, g_raw);
    g_frames++;
    return 0;
}

int host_frame_count(void) { return g_frames; }

void host_close(void)
{
    if (g_raw) {
        fclose(g_raw);
        g_raw = NULL;
    }
}

#endif /* ARDUINO */
