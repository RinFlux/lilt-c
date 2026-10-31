/* ll_input.h — 按键模型（移植自 lilt 的 ui/input.go 与 ui/keypoll.go）
 *
 * 框架只认"逻辑键"，不认引脚。谁读电平都行：真按键、串口、编码器、IO 扩展。
 *
 * 三个关键约定，lilt 的手感全建立在这上面：
 *   1) 一次按键要发 Press 和 Release 两个事件；
 *   2) 动作在 Release（且未被拖动取消）或 Repeat 时触发，Press 只给视觉反馈；
 *   3) 按住进入连发，间隔每次乘以 Accel 越按越快。
 */
#ifndef LL_INPUT_H
#define LL_INPUT_H

#include <stdint.h>

typedef enum {
    LL_KEY_UP = 0,
    LL_KEY_DOWN,
    LL_KEY_LEFT,
    LL_KEY_RIGHT,
    LL_KEY_OK,        /* 摇杆按下 / 空格 */
    LL_KEY_BACK,      /* 返回上一屏 / 关弹窗 */
    LL_KEY_MENU,      /* 框架不处理，留给应用 */
    LL_KEY_BACKSPACE,
    LL_KEY_CHAR,      /* 字符输入，码点在 rune 里 */
    LL_KEY_COUNT
} LlKey;

typedef enum {
    LL_PHASE_PRESS = 0,
    LL_PHASE_REPEAT,
    LL_PHASE_RELEASE
} LlPhase;

typedef struct {
    LlKey    key;
    LlPhase  phase;
    uint32_t rune;    /* 只有 LL_KEY_CHAR 用 */
    int      canceled;/* 触摸变成拖动后，那次 Release 会被取消 */
} LlKeyEvent;

/* 动作触发规则：松开且没取消，或者连发中的每一次。 */
int ll_event_fires(const LlKeyEvent *e);

void ll_input_init(void);

/* 入队一帧要处理的事件；队列满时丢弃（界面卡住时不反过来阻塞读引脚的人）。 */
int  ll_input_emit(LlKey k, LlPhase p);
int  ll_input_emit_rune(uint32_t r);
int  ll_input_emit_canceled(LlKey k, LlPhase p);
/* 帧循环开头取出事件，取完返回 0。 */
int  ll_input_pop(LlKeyEvent *out);
int  ll_input_queued(void);

/* ---- 消抖 + 连发（对应 ui.KeyPoller） ---- */
typedef struct {
    int     raw, down;    /* raw = 最近采样，down = 消抖后的状态 */
    uint8_t stable;       /* 当前电平连续一致的次数 */
    float   held, next, iv; /* 按住时长、下次连发时刻、当前连发间隔 */
} LlPollState;

typedef struct {
    uint8_t     debounce;        /* 连续几次采样一致才算状态改变 */
    float       repeat_delay;    /* 按住多少秒后开始连发 */
    float       repeat_interval; /* 第一次连发的间隔 */
    float       min_interval;    /* 间隔下限 */
    float       accel;           /* 每连发一次，间隔乘以它 */

    LlPollState keys[LL_KEY_COUNT];
} LlKeyPoller;

void ll_keypoll_init(LlKeyPoller *p);   /* 2 / 0.2 / 0.07 / 0.03 / 0.9 */
/* 每个键每轮喂一次采样：down 是此刻是否按着，dt 是距上一轮的秒数。 */
void ll_keypoll_poll(LlKeyPoller *p, LlKey k, int down, float dt);

/* ---- 串口当按键（现在的输入方式，接真按键后可整块删掉） ----
 * 规则：小写字母 = 按下该键，大写字母 = 立刻松开；终端自动重复会持续喂"按着"，
 * 超过 hold_timeout 没有新字节就当作松手，所以点按和长按都能正常出来。 */
typedef struct {
    LlKeyPoller poller;
    float hold_timeout;
    float held_for[LL_KEY_COUNT];
    int   armed[LL_KEY_COUNT];
} LlSerialKeys;

void ll_serial_keys_init(LlSerialKeys *sk);
void ll_serial_keys_service(LlSerialKeys *sk, float dt); /* 每帧调一次：读串口 + 喂 poller */

#endif /* LL_INPUT_H */
