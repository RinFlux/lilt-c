/* ll_app.c — 帧循环实现 */
#include <stddef.h>
#include "ll_app.h"
#include "ll_anim.h"
#include "ll_conf.h"
#include "ll_bus.h"
#include "ll_screen.h"
#include "ll_theme.h"

int ll_app_fps = LL_MAX_FPS;

static LlScreen *g_screen;
static LlScene  *g_scene;
static int       g_running;
static uint32_t  g_last_ms;
static int       g_dirty;
static int       g_captured[LL_KEY_COUNT];   /* 谁认领了这个键的 Press */

typedef struct { LlDoFn fn; void *user; } LlDoItem;
static LlDoItem g_do[LL_UI_DO_QUEUE];
static int      g_do_n;

void ll_app_invalidate(void) { g_dirty = 1; }

int ll_app_do(LlDoFn fn, void *user)
{
    if (g_do_n >= LL_UI_DO_QUEUE) {
        return 0;
    }
    g_do[g_do_n].fn   = fn;
    g_do[g_do_n].user = user;
    g_do_n++;
    return 1;
}

/* 在两帧之间执行排队的界面改动；fn 里不允许再调用 ll_app_do（和 lilt 一样）。 */
static void run_pending_do(void)
{
    int n = g_do_n;

    for (int i = 0; i < n; i++) {
        LlDoFn fn   = g_do[i].fn;
        void  *user = g_do[i].user;

        g_do[i].fn = NULL;
        g_do[i].user = NULL;
        fn(user);
    }
    g_do_n = 0;
}

LlCanvas ll_app_canvas(void) { return ll_canvas_new(g_screen); }
LlScene *ll_app_scene(void)  { return g_scene; }

void ll_app_boot(LlScreen *screen, LlScene *scene)
{
    g_screen  = screen;
    g_scene   = scene;
    g_running = 1;
    g_dirty   = 1;
    g_do_n    = 0;
    for (int i = 0; i < LL_KEY_COUNT; i++) {
        g_captured[i] = 0;
    }
    ll_anim_init();
    ll_input_init();
    g_last_ms = ll_bus_millis();
}

void ll_app_pause(void) { g_running = 0; }

void ll_app_stop(void)
{
    g_running = 0;
    g_do_n    = 0;
    ll_anim_init();
    ll_input_init();
}

/* 输入分发：Press 先到先得并被"认领"，之后的 Repeat/Release 只给认领者。 */
static void dispatch_input(void)
{
    LlKeyEvent ev;

    while (ll_input_pop(&ev)) {
        if (ev.key >= LL_KEY_COUNT) {
            continue;
        }
        if (ev.phase == LL_PHASE_PRESS) {
            if (g_captured[ev.key]) {
                continue;               /* 已经有主，这次按下面向的对象不该收到 */
            }
            if (g_scene && g_scene->key && g_scene->key(g_scene, &ev)) {
                g_captured[ev.key] = 1;
            }
            continue;
        }
        /* Repeat / Release：没有认领者就当成"框架默认行为"，交给场景处理一次 */
        if (g_scene && g_scene->key) {
            g_scene->key(g_scene, &ev);
        }
        if (ev.phase == LL_PHASE_RELEASE) {
            g_captured[ev.key] = 0;
        }
    }
}

int ll_app_step(float dt)
{
    int moved;

    run_pending_do();
    dispatch_input();

    if (g_scene && g_scene->update) {
        g_scene->update(g_scene, dt);
    }
    /* 顺序刻意是"更新 -> 动画"：更新里设的目标，这一帧就开始走。 */
    moved = ll_anim_step(dt);

    if (!g_dirty && !moved && !ll_screen_dirty_mask()) {
        return 0;                       /* 什么都没变：不画、不推总线 */
    }
    g_dirty = 0;

    ll_screen_clear(ll_theme.background);
    if (g_scene && g_scene->paint) {
        g_scene->paint(g_scene, ll_app_canvas());
    }
    ll_screen_flush();
    return 1;
}

int ll_app_frame(void)
{
    uint32_t now, last;
    float    dt;
    int      period_ms, spent_ms, repainted;

    if (!g_running) {
        return 0;
    }
    now     = ll_bus_millis();
    last    = g_last_ms;
    g_last_ms = now;

    dt = (float)(now - last) / 1000.0f;
    if (dt > 0.1f) {          /* 卡了一下也不要让动画一口气跳到终点 */
        dt = 0.1f;
    }
    repainted = ll_app_step(dt);

    period_ms = 1000 / (ll_app_fps > 0 ? ll_app_fps : 1);
    spent_ms  = (int)(ll_bus_millis() - now);
    if (spent_ms < period_ms) {
        ll_bus_delay(period_ms - spent_ms);
    }
    return repainted;
}
