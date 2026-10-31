/* lilt_c.h — 给 C++（Arduino 的 .ino/.cpp）用的总包含头
 *
 * 引擎本身是纯 C，头文件里没有 extern "C" 标记；与其在十一个头里各加一遍，
 * 不如在这里统一包一层：C++ 编译时把这些声明放进 C 链接上下文，
 * 这样 .ino 里 #include "lilt_c.h" 一句就够了。
 *
 * 注意：三个 demo_*.h 不在这个包裹里（它们是被 .ino 单独 #include 的），
 * 所以那三个头各自带 extern "C" 标记。少一处就会在链接期炸出
 * "undefined reference to 'll_tour_reset()'"——注意报错里的括号，那是 C++
 * 被改写过的名字，和 .c 里那个 C 名字根本不是同一个符号。
 */
#ifndef LILT_C_H
#define LILT_C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ll_anim.h"
#include "ll_app.h"
#include "ll_bus.h"
#include "ll_conf.h"
#include "ll_cursor.h"
#include "ll_font.h"
#include "ll_geom.h"
#include "ll_gfx.h"
#include "ll_input.h"
#include "ll_node.h"
#include "ll_page.h"
#include "ll_screen.h"
#include "ll_theme.h"

#ifdef __cplusplus
}
#endif

#endif /* LILT_C_H */
