/* ll_anim.c — 弹簧动画引擎实现
 *
 * 对应 lilt 的 anim/spring.go + anim/value.go + anim/scheduler.go。
 * 常量和算法刻意保持原样（连 twoPi、e 的 float32 近似值都没改），
 * 这样移植出来的手感才有可比性。
 */
#include "ll_anim.h"

/* 子步长：半隐式欧拉在低帧率下遇到硬弹簧会发散，固定 1/240 秒一步保证稳定。 */
#define SUBSTEP (1.0f / 240.0f)
#define TWO_PI  6.2831855f
#define E_CONST 2.7182817f

float ll_anim_time_scale = 1.0f;
float ll_rubber_band_coeff = 0.55f;

/* ---- 预设。都是指针，运行时改 ll_spring_default->response 全局生效 ---- */
static LlSpring g_spring_default     = { 0.29f, 0.75f, 0 };
static LlSpring g_spring_smooth      = { 0.39f, 0.85f, 0 };
static LlSpring g_spring_snappy      = { 0.31f, 0.80f, 0 };
static LlSpring g_spring_bouncy      = { 0.25f, 0.45f, 0 };
static LlSpring g_spring_interactive = { 0.14f, 1.00f, 0 };

LlSpring *const ll_spring_default     = &g_spring_default;
LlSpring *const ll_spring_smooth      = &g_spring_smooth;
LlSpring *const ll_spring_snappy      = &g_spring_snappy;
LlSpring *const ll_spring_bouncy      = &g_spring_bouncy;
LlSpring *const ll_spring_interactive = &g_spring_interactive;

/* ---- 活跃表：只挂"正在动"的值，静止即摘除（对应 anim/scheduler.go） ---- */
static LlValue *g_active[LL_ANIM_MAX_VALUES];
static int      g_active_n;
static int      g_overflow;

static float f_abs(float x) { return x < 0.0f ? -x : x; }

void ll_anim_init(void)
{
    for (int i = 0; i < g_active_n; i++) {
        g_active[i] = 0;
    }
    g_active_n = 0;
    g_overflow = 0;
}

int ll_anim_active_count(void)   { return g_active_n; }
int ll_anim_overflow_count(void) { return g_overflow; }
int ll_anim_busy(void)           { return g_active_n != 0; }

/* 角频率（ease 时是缓动速率），单位 1/s。 */
static float spring_rate(const LlSpring *s)
{
    float r = s->response;
    /* lilt 假定 response 是正数；这里加一道下限，避免 0 造成除零变成 inf/NaN。 */
    if (r < 0.001f) {
        r = 0.001f;
    }
    return TWO_PI / r;
}

static void spring_coefficients(const LlSpring *s, float *k, float *c)
{
    float w = spring_rate(s);
    *k = w * w;
    *c = 2.0f * s->damping * w;
}

/* e^(-x) 的近似，x >= 0。
 * ESP32-C3 没有 FPU，float64 的 math.exp 要走软件实现，所以按 lilt 的做法：
 * 把指数不断折半到很小，用 (2,2) Padé 近似算一次，再平方回去。 */
static float exp_neg(float x)
{
    int n = 0;
    float x2, h, r;

    if (x <= 0.0f) {
        return 1.0f;
    }
    if (x > 16.0f) {
        return 0.0f;
    }
    while (x > 0.5f) {
        x *= 0.5f;
        n++;
    }
    x2 = x * x / 12.0f;
    h  = x / 2.0f;
    r  = (1.0f - h + x2) / (1.0f + h + x2);
    while (n > 0) {
        r *= r;
        n--;
    }
    return r;
}

const LlSpring *ll_value_curve(const LlValue *v)
{
    return v->spring ? v->spring : ll_spring_default;
}

static void value_wake(LlValue *v)
{
    if (v->active) {
        return;
    }
    if (g_active_n >= LL_ANIM_MAX_VALUES) {
        /* 表满：直接吸附到目标，宁可丢一段动画也不能让界面挂住。
         * 出现这个说明 LL_ANIM_MAX_VALUES 设小了，看 ll_anim_overflow_count()。 */
        g_overflow++;
        v->current = v->target;
        v->velocity = 0.0f;
        return;
    }
    v->active = 1;
    g_active[g_active_n++] = v;
}

void ll_value_init(LlValue *v, float val, const LlSpring *spring)
{
    v->current   = val;
    v->target    = val;
    v->velocity  = 0.0f;
    v->precision = 0.0f;   /* 0 表示用默认 0.05 */
    v->spring    = spring;
    v->parent    = 0;
    v->active    = 0;
}

float ll_value_get(const LlValue *v)
{
    if (v->parent) {
        return v->current + ll_value_get(v->parent);
    }
    return v->current;
}

float ll_value_current(const LlValue *v) { return v->current; }
float ll_value_target(const LlValue *v)  { return v->target; }
float ll_value_velocity(const LlValue *v) { return v->velocity; }
int   ll_value_settled(const LlValue *v)  { return !v->active; }

void ll_value_set(LlValue *v, float x)
{
    if (x == v->target) {
        return;              /* 目标没变就不要白白唤醒 */
    }
    v->target = x;
    value_wake(v);
}

void ll_value_jump(LlValue *v, float x)
{
    v->current = x;
    v->target  = x;
    v->velocity = 0.0f;
}

void ll_value_shift(LlValue *v, float d)
{
    if (d == 0.0f) {
        return;
    }
    v->current += d;
    value_wake(v);
    /* 注意目标不变：于是它会"滑回去"。布局位置变化就靠这个动。 */
}

void ll_value_kick(LlValue *v, float velocity)
{
    v->velocity += velocity;
    value_wake(v);
}

void ll_value_bump(LlValue *v, float px)
{
    const LlSpring *s = ll_value_curve(v);
    ll_value_kick(v, px * spring_rate(s) * E_CONST);
}

void ll_value_set_spring(LlValue *v, const LlSpring *s)   { v->spring = s; }
void ll_value_set_parent(LlValue *v, LlValue *parent)     { v->parent = parent; }
void ll_value_set_precision(LlValue *v, float p)         { v->precision = p; }

/* 单值的时间推进。 */
static void value_step(LlValue *v, float dt)
{
    const LlSpring *s = ll_value_curve(v);
    float x = v->current - v->target;
    float vel = v->velocity;

    if (s->ease) {
        /* 指数缓动：全力出发、没有动量、不回弹。Damping 被忽略。 */
        x = x * exp_neg(spring_rate(s) * dt);
        vel = 0.0f;
    } else {
        float k, c;
        int n, i;
        float h;

        spring_coefficients(s, &k, &c);
        n = (int)(dt / SUBSTEP) + 1;
        h = dt / (float)n;
        for (i = 0; i < n; i++) {
            vel += (-k * x - c * vel) * h;   /* 加速度 = -kx - cv */
            x   += vel * h;                  /* 先更新速度再更新位移 = 半隐式 */
        }
    }

    float p = v->precision;
    if (p == 0.0f) {
        p = 0.05f;   /* 像素尺度够用；0..1 的进度要另外设小一点 */
    }
    if (f_abs(x) < p && f_abs(vel) < p * 10.0f) {
        v->current  = v->target;
        v->velocity = 0.0f;
        v->active   = 0;
        return;
    }
    v->current  = v->target + x;
    v->velocity = vel;
}

int ll_anim_step(float dt)
{
    int i;

    if (g_active_n == 0) {
        return 0;
    }
    dt *= ll_anim_time_scale;
    for (i = 0; i < g_active_n; ) {
        LlValue *v = g_active[i];
        value_step(v, dt);
        if (v->active) {
            i++;
            continue;
        }
        /* 静止：用最后一个补上这个位置，容量不缩（lilt 会缩 slice，C 里等价）。 */
        g_active_n--;
        g_active[i] = g_active[g_active_n];
        g_active[g_active_n] = 0;
    }
    return 1;
}

float ll_rubber_band(float x, float dim)
{
    int   neg;
    float y;

    if (dim <= 0.0f) {
        return 0.0f;
    }
    neg = x < 0.0f;
    if (neg) {
        x = -x;
    }
    y = (1.0f - 1.0f / (x * ll_rubber_band_coeff / dim + 1.0f)) * dim;
    return neg ? -y : y;
}
