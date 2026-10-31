/* ll_app.h — 帧循环与生命周期（对应 lilt 的 lilt.go + internal/core）
 *
 * 每帧严格按 lilt 的顺序走：
 *   输入 -> 更新 -> 动画 -> 布局 -> 绘制 -> Flush
 * 没变化就不画也不推总线，所以静止界面几乎不耗 CPU。
 *
 * A 阶段先用一个最小的"场景"接口（三张函数指针）；B 阶段的节点树本身
 * 就会实现成 LlScene，应用代码的写法不变。
 */
#ifndef LL_APP_H
#define LL_APP_H

#include "ll_gfx.h"
#include "ll_input.h"

typedef struct LlScene {
    const char *name;
    /* 每帧一次逻辑；要动动画就调 ll_value_set/ll_value_kick。 */
    void (*update)(struct LlScene *self, float dt);
    /* 把这一帧画进 canvas（已裁剪到整屏）。 */
    void (*paint)(struct LlScene *self, LlCanvas canvas);
    /* 按键事件；返回 1 表示这个键被我认领了：它后续的连发和松开只给我。 */
    int  (*key)(struct LlScene *self, const LlKeyEvent *ev);
    void *user;
} LlScene;

/* 帧率上限，每帧都读，运行中可改。 */
extern int ll_app_fps;

/* 标脏：下一帧即使没有动画也会重排 + 重画（对应"任何属性变化都会标记重新布局"）。 */
void ll_app_invalidate(void);

/* 在两帧之间执行改界面的代码。后台回调要动界面时必须走这里，
 * 不能直接改（对应 lilt 的 ui.Do）。返回 0 表示队列满了。 */
typedef void (*LlDoFn)(void *user);
int ll_app_do(LlDoFn fn, void *user);

/* 启动：绑定屏幕和第一个场景。paused 状态下再调用等于恢复。 */
void ll_app_boot(LlScreen *screen, LlScene *scene);
void ll_app_pause(void);
void ll_app_stop(void);

/* 跑一帧（自己按 millis 算 dt，并按 ll_app_fps 限速）。
 * 返回 1 = 这帧重画并推送了。把它放在 loop() 里反复调用即可。 */
int ll_app_frame(void);

/* 手动推进一帧，dt 自己给。主机录图时用这个，能保证每帧落在固定时间点上。 */
int ll_app_step(float dt);

LlCanvas ll_app_canvas(void);
LlScene *ll_app_scene(void);

#endif /* LL_APP_H */
