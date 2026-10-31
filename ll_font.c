/* ll_font.c — 字体度量与 UTF-8 解码 */
#include "ll_font.h"

int16_t ll_font_ascent(const LlFont *f)
{
    /* Ascent = -BBox[3]（BBox 的第四项是最小的 y，即基线以上的最高距离） */
    return (int16_t)(-f->bbox[3]);
}

int16_t ll_font_line_height(const LlFont *f)
{
    int16_t a = (int16_t)f->y_advance;
    int16_t b = (int16_t)f->bbox[1];
    return a > b ? a : b;
}

uint32_t ll_font_utf8_next(const char **sp)
{
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = s[0];

    if (c == 0u) {
        return 0u;
    }
    if (c < 0x80u) {
        *sp += 1;
        return c;
    }
    if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        *sp += 2;
        return ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    }
    if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *sp += 3;
        return ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6)
             | (uint32_t)(s[2] & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u
        && (s[3] & 0xC0u) == 0x80u) {
        *sp += 4;
        return ((uint32_t)(c & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12)
             | ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
    }
    /* 非法字节：跳过一个字节，别让循环卡住 */
    *sp += 1;
    return 0xFFFDu;
}

int16_t ll_font_text_width(const LlFont *f, const char *s)
{
    int16_t w = 0;
    const char *p = s;

    while (p && *p) {
        uint32_t r = ll_font_utf8_next(&p);
        w += (int16_t)tf_font_advance(f, r);
    }
    return w;
}
