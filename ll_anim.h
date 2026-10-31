/* ll_anim.h — 弹簧动画引擎（lilt 的 anim 包逐行移植，数值未改）
 *
 * 整个框架只有这一个动画原语：LlValue 是一个"会自己走向目标值的 float"。
 * 光标、列表每一行、页面位移、布局变化、超滚橡皮筋……全都是它，
 * 这就是 lilt 手感统一的原因。
 *
 * 物理模型：半隐式欧拉（semi-implicit Euler），每帧按 1/240 秒的子步细分，
 * 参数用 SwiftUI 的写法——response 是无阻尼振荡周期（秒），damping 是
 * 临界阻尼比例（1 = 不回弹，越小越弹）。
 */
#ifndef LL_ANIM_H
#define LL_ANIM_H

#include <stdint.h>
#include "ll_conf.h"

typedef struct {
    float response;  /* 振荡周期，秒 */
    float damping;   /* 阻尼比 */
    int   ease;      /* 非 0 时放弃弹簧，改指数缓动：一帧就全力出发，不回弹 */
} LlSpring;

/* 精度默认 0（表示用 0.05），与 lilt 里"零值即可用"的写法一致。 */
typedef struct LlValue {
    float            current;
    float            target;
    float            velocity;   /* 单位/秒 */
    float            precision;  /* 小于它就认为静止 */
    const LlSpring  *spring;     /* NULL = 用 ll_spring_default */
    struct LlValue  *parent;     /* 非空则 Get() 叠加父值，用于转场偏移继承 */
    int              active;     /* 是否挂在活跃表里 */
} LlValue;

/* ---- 预设：指针，改一处全局生效（lilt 靠这个做"手感总控"） ---- */
extern LlSpring *const ll_spring_default;     /* 0.29 / 0.75 列表与布局，轻微过冲 */
extern LlSpring *const ll_spring_smooth;      /* 0.39 / 0.85 整页进出 */
extern LlSpring *const ll_spring_snappy;      /* 0.31 / 0.8  拇指、进度 */
extern LlSpring *const ll_spring_bouncy;      /* 0.25 / 0.45 标记弹入，落点有脆响 */
extern LlSpring *const ll_spring_interactive; /* 0.14 / 1    跟手指、按下反馈 */

/* 全局时间缩放，调慢一点方便用眼睛看清弹簧怎么走的。每帧都读。 */
extern float ll_anim_time_scale;

/* 橡皮筋阻力系数，与 UIScrollView 的超滚阻尼一致。 */
extern float ll_rubber_band_coeff;

/* 初始化/复位（对应 lilt 的 Start 之前状态干净；Stop 会调它）。 */
void ll_anim_init(void);

/* 等价于 anim.NewValue(v, spring)：就地初始化，静止在 v。 */
void ll_value_init(LlValue *v, float val, const LlSpring *spring);

/* 显示值，包含父值贡献（画出来、量出来的都是它）。 */
float ll_value_get(const LlValue *v);
float ll_value_current(const LlValue *v);  /* 不含父值 */
float ll_value_target(const LlValue *v);
float ll_value_velocity(const LlValue *v);
int   ll_value_settled(const LlValue *v);   /* 1 = 静止 */

void ll_value_set(LlValue *v, float x);            /* 朝 x 动画 */
void ll_value_jump(LlValue *v, float x);           /* 立刻到 x 并停住 */
void ll_value_shift(LlValue *v, float d);          /* 当前值平移 d，目标不变 -> 滑回去 */
void ll_value_kick(LlValue *v, float velocity);    /* 给一个速度冲量 */
void ll_value_bump(LlValue *v, float px);          /* 静止时给它一冲，摆约 px 再回来 */

void ll_value_set_spring(LlValue *v, const LlSpring *s);
void ll_value_set_parent(LlValue *v, LlValue *parent);
void ll_value_set_precision(LlValue *v, float p);

/* 推进所有"正在动"的值；返回 1 表示有值动了（调用方据此决定要不要重画）。
 * 静止的值不在表里，所以静止界面零开销。 */
int ll_anim_step(float dt);

/* 还有东西在动吗？ */
int ll_anim_busy(void);

/* 活跃表里当前挂了多少个值（诊断用）。 */
int ll_anim_active_count(void);

/* 溢出被"直接吸附到目标"的次数；正常应该一直是 0，涨了说明 LL_ANIM_MAX_VALUES 太小。 */
int ll_anim_overflow_count(void);

/* 把原始超滚距离映射成显示距离：越推越沉，永远到不了 dim。 */
float ll_rubber_band(float x, float dim);

#endif /* LL_ANIM_H */
