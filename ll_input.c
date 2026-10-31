/* ll_input.c — 事件队列、消抖轮询、串口按键 */
#include "ll_input.h"
#include "ll_bus.h"
#include "ll_conf.h"

/* ---------------- 事件队列（容量与 lilt 一致：32） ----------------
 *
 * 板子上这是"单生产者 / 单消费者"环：按键任务只写 tail，帧循环只写 head，
 * 两边读对方的下标时即使读到旧值也只是"少看一个"，不会读到半个事件。
 * 所以不需要临界区——原来那份用 g_count 的写法两边都会改它，
 * 在 FreeRTOS 抢占下会丢事件，现在这版才是真能上板的。
 * 事件本身是 16 字节的 struct，一次赋值整体写入，消费者只会在 tail 前进之后才看到它。 */

static LlKeyEvent    g_queue[LL_INPUT_QUEUE];
static volatile int  g_head;   /* 消费者（帧循环）写 */
static volatile int  g_tail;   /* 生产者（读按键的一方）写 */

int ll_event_fires(const LlKeyEvent *e)
{
    if (e->phase == LL_PHASE_REPEAT) {
        return 1;
    }
    return e->phase == LL_PHASE_RELEASE && !e->canceled;
}

void ll_input_init(void)
{
    g_head = 0;
    g_tail = 0;
}

/* 还剩几个空位（按"留一格区分满/空"的经典做法，容量实际是 LL_INPUT_QUEUE-1）。 */
static int free_slots(void)
{
    return (g_head - g_tail - 1 + 2 * LL_INPUT_QUEUE) % LL_INPUT_QUEUE;
}

static int enqueue(LlKeyEvent *e)
{
    int next;

    if (free_slots() <= 0) {
        return 0;              /* 满了就丢，绝不阻塞读引脚的一方 */
    }
    next = g_tail;
    g_queue[next] = *e;
    g_tail = (next + 1) % LL_INPUT_QUEUE;   /* tail 前进 = 发布，必须在写内容之后 */
    return 1;
}

int ll_input_emit(LlKey k, LlPhase p)
{
    LlKeyEvent e = { k, p, 0u, 0 };
    return enqueue(&e);
}

int ll_input_emit_canceled(LlKey k, LlPhase p)
{
    LlKeyEvent e = { k, p, 0u, 1 };
    return enqueue(&e);
}

int ll_input_emit_rune(uint32_t r)
{
    LlKeyEvent e = { LL_KEY_CHAR, LL_PHASE_PRESS, r, 0 };
    return enqueue(&e);
}

int ll_input_pop(LlKeyEvent *out)
{
    if (g_head == g_tail) {
        return 0;
    }
    *out = g_queue[g_head];
    g_head = (g_head + 1) % LL_INPUT_QUEUE;
    return 1;
}

int ll_input_queued(void) { return (g_tail - g_head + LL_INPUT_QUEUE) % LL_INPUT_QUEUE; }

/* ---------------- KeyPoller：消抖 + 加速连发（逐行对应 ui/keypoll.go） ---------------- */

void ll_keypoll_init(LlKeyPoller *p)
{
    p->debounce        = 2;
    p->repeat_delay    = 0.2f;
    p->repeat_interval = 0.07f;
    p->min_interval    = 0.03f;
    p->accel           = 0.9f;
    for (int i = 0; i < LL_KEY_COUNT; i++) {
        p->keys[i].raw = p->keys[i].down = 0;
        p->keys[i].stable = 0;
        p->keys[i].held = p->keys[i].next = p->keys[i].iv = 0.0f;
    }
}

void ll_keypoll_poll(LlKeyPoller *p, LlKey k, int down, float dt)
{
    LlPollState *s = &p->keys[k];

    if (down != s->raw) {
        s->raw = down;
        s->stable = 0;
    } else if (s->stable < p->debounce) {
        s->stable++;
    }
    if (s->stable >= p->debounce && s->raw != s->down) {
        s->down = s->raw;
        if (s->down) {
            s->held = 0.0f;
            s->next = p->repeat_delay;
            s->iv   = p->repeat_interval;
            ll_input_emit(k, LL_PHASE_PRESS);
        } else {
            ll_input_emit(k, LL_PHASE_RELEASE);
        }
        return;
    }
    if (!s->down) {
        return;
    }
    s->held += dt;
    if (s->held >= s->next) {
        ll_input_emit(k, LL_PHASE_REPEAT);
        s->next += s->iv;
        s->iv *= p->accel;
        if (s->iv < p->min_interval) {
            s->iv = p->min_interval;
        }
    }
}

/* ---------------- 串口当按键 ----------------
 * 小写字母按下、大写字母立刻松开；终端的自动重复相当于"一直按着"，
 * 超过 hold_timeout 没有新字节则视为松手。接真按键时把这块换成
 * ll_keypoll_poll(&poller, k, digitalRead(pin), dt) 即可，其余不动。 */

typedef struct { uint8_t press; uint8_t release; LlKey key; } KeyMap;

static const KeyMap g_map[] = {
    { 'w', 'W', LL_KEY_UP    },
    { 's', 'S', LL_KEY_DOWN  },
    { 'a', 'A', LL_KEY_LEFT  },
    { 'd', 'D', LL_KEY_RIGHT },
    { ' ', 'x', LL_KEY_OK    },   /* 空格按下，x 松开；回车也算按下 */
    { '\r', 'x', LL_KEY_OK  },
    { '\n', 'x', LL_KEY_OK  },
    { 'j', 'J', LL_KEY_BACK  },
    { 'k', 'K', LL_KEY_MENU  },
    { 'h', 'H', LL_KEY_BACKSPACE },
};
#define LL_MAP_COUNT ((int)(sizeof(g_map) / sizeof(g_map[0])))

static const LlKey g_poll_order[] = {
    LL_KEY_UP, LL_KEY_DOWN, LL_KEY_LEFT, LL_KEY_RIGHT,
    LL_KEY_OK, LL_KEY_BACK, LL_KEY_MENU, LL_KEY_BACKSPACE
};
#define LL_POLL_COUNT ((int)(sizeof(g_poll_order) / sizeof(g_poll_order[0])))

void ll_serial_keys_init(LlSerialKeys *sk)
{
    ll_keypoll_init(&sk->poller);
    sk->hold_timeout = 0.09f;   /* 松手判定的等待，见上面注释 */
    for (int i = 0; i < LL_KEY_COUNT; i++) {
        sk->held_for[i] = 1e9f;
        sk->armed[i]    = 0;
    }
}

static int sk_index(LlKey k)
{
    for (int i = 0; i < LL_POLL_COUNT; i++) {
        if (g_poll_order[i] == k) return i;
    }
    return -1;
}

void ll_serial_keys_service(LlSerialKeys *sk, float dt)
{
    int c;
    while ((c = ll_bus_read_key_byte()) >= 0) {
        int matched = 0;

        for (int i = 0; i < LL_MAP_COUNT; i++) {
            int idx;

            if (g_map[i].press != (uint8_t)c && g_map[i].release != (uint8_t)c) {
                continue;
            }
            matched = 1;
            idx = sk_index(g_map[i].key);
            if (idx < 0) {
                continue;
            }
            if (g_map[i].press == (uint8_t)c) {
                sk->held_for[idx] = 0.0f;   /* 重新计时 = 还按着 */
                sk->armed[idx]    = 1;
            } else {
                sk->held_for[idx] = 1e9f;   /* 显式松开 */
                sk->armed[idx]    = 0;
            }
        }
        if (!matched && c >= 0x20 && c < 0x7f) {
            ll_input_emit_rune((uint32_t)c);   /* 给 Input 组件当字符 */
        }
    }

    for (int i = 0; i < LL_POLL_COUNT; i++) {
        int   down;
        LlKey k = g_poll_order[i];

        if (sk->armed[i]) {
            sk->held_for[i] += dt;
        }
        down = sk->armed[i] && sk->held_for[i] < sk->hold_timeout;
        if (!down) {
            sk->armed[i] = 0;
        }
        ll_keypoll_poll(&sk->poller, k, down, dt);
    }
}
