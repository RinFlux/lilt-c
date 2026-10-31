# 实现细节

README 讲怎么用，这里讲里面是怎么搭的。想改手感、加控件、或者把它挪到别的板子上，先看这一篇。

## 一支弹簧撑起全部动画

引擎里只有一个会动的东西，`LlValue`。它存当前值、目标值、速度、静止阈值，外加一个指向弹簧参数的指针和一个可选的父值指针（父值用来继承页面转场的偏移）。

```c
typedef struct LlValue {
    float            current;
    float            target;
    float            velocity;   /* 单位/秒 */
    float            precision;  /* 小于它就认为静止 */
    const LlSpring  *spring;     /* NULL = 用 ll_spring_default */
    struct LlValue  *parent;     /* 非空则 Get() 叠加父值 */
    int              active;     /* 是否挂在活跃表里 */
} LlValue;
```

积分用半隐式欧拉，固定子步长 1/240 秒。低帧率下硬弹簧会发散，把一帧切成若干个 1/240 的小步就稳了；帧率再低，手感也不会变，只是采样点稀一些。

弹簧参数沿用 SwiftUI 的 Response/Damping 语义，`response` 大致是"多久到位"，`damping` 是阻尼，1.0 是不回弹的临界阻尼，越小越弹。四组预设都是全局指针，改一次全树生效：

```c
ll_spring_default->response = 0.39f;    /* Default .29/.75  Smooth .39/.85 */
ll_spring_default->damping  = 0.85f;    /* Snappy  .31/.80  Bouncy .25/.45 */
```

Gallery 里 MENU 键换"手感档位"就是这么实现的，没有第二套动画系统，改的就是这一个结构体。Tour 结尾那段 A/B 对比同理，切 0.75 和 0.45 两个阻尼。

用法是 `ll_value_init(&v, 起始, 弹簧)`、`ll_value_set(&v, 目标)`、每帧 `ll_anim_step(dt)`、`ll_value_get(&v)`。另外几个动作各管一件事：`ll_value_jump` 瞬间到位且清掉动量，列表"内容跳"用它；`ll_value_shift` 只挪当前值不挪目标，所以它会自己滑回去，波浪和橡皮筋都靠它；`ll_value_kick` 给一记冲量；`ll_value_bump` 顶一下。

`Ease` 那条曲线是指数衰减，用 (2,2) 阶 Padé 近似的 `exp_neg` 算，比 `expf` 便宜，而且不会像弹簧那样回弹。自检里有一条专门盯它。

## 写一页

一个页面就是一个全局 `LlPage` 加一个 `build()`。框架负责根容器、滚动视口、进/出场动画、焦点与光标、方向键走焦、OK 触发、Back 出栈。

```c
#include "lilt_c.h"

/* 文案的缓冲区必须活得比节点久，节点只存指针，不复制字符串。 */
static char    g_row[3][12] = { "apple", "banana", "cherry" };
static char    g_count_text[16] = "count 0";
static LlNode *g_count_node;
static int     g_count;

static void pick(void *user)                 /* user 就是传进去的那个整数 */
{
    g_count += (int)(intptr_t)user;
    snprintf(g_count_text, sizeof(g_count_text), "count %d", g_count);
    ll_node_set_text(g_count_node, g_count_text);
}

static void on_invert(void *user)
{
    LlColor b = ll_theme.background;

    (void)user;
    ll_theme.background = ll_theme.foreground;
    ll_theme.foreground = b;
    ll_app_invalidate();                     /* 告诉帧循环这帧要重画 */
}

static void build(LlPage *p)
{
    ll_page_put(p, ll_label_small("my first page"));
    for (int i = 0; i < 3; i++) {
        ll_page_put(p, ll_list_item(g_row[i], pick, (void *)(intptr_t)1));
    }
    g_count_node = ll_label(g_count_text);
    ll_page_put(p, g_count_node);
    ll_page_put(p, ll_check_box("invert", 0, on_invert, NULL));
}

LlPage ll_page_mine = { .name = "mine", .build = build };

void setup(void)
{
    ll_screen_init();
    ll_pages_boot(ll_screen_api(), &ll_page_mine);
}
```

这段是照着能编译的写法给的，`gcc -Wall -Wextra` 过。

控件有 `ll_label`、`ll_label_small`、`ll_list_item`（带右侧 `>`）、`ll_button`、`ll_check_box`，容器有 `ll_column`、`ll_row`、`ll_list`、`ll_scroll(content, 0)`。间距按"单位"算，`ll_set_padding_u(n, ll_insets_xy(4, 1))` 里一个单位等于默认字体行高的 1/15，换字体时比例不会崩。

回调里别直接改界面。回调可能跑在按键任务里，和帧循环并发。规矩和 lilt 一样，把改动排进队列，下一帧开头统一执行：

```c
static void on_plus(void *user) { ll_app_do(pick, (void *)(intptr_t)1); }
```

`ll_app_do(fn, user)` 只入队，`ll_app_step()` 在帧首清空它。"点了但要等一帧才变"是设计好的行为，自检里有一条在验证。

`LlPage` 的结构是 `Column(root)` 套 `Scroll` 套 `Column(content)`。滚动容器 `grow=1` 且主轴首选尺寸为 0，所以往 `p->root` 上再挂一个孩子，那个孩子就变成钉在底部的页脚，列表滚到哪都不会把它滚走。Tour 的字幕就是这么放的。

节点在 `ll_node_new` 时打上 `gen` 标记，页面出场时用 `ll_node_free_from(gen)` 整批回收。自检里有一条连演两圈巡演然后比对节点池占用，防止漏回收。

## 帧循环

`ll_app_step(dt)` 的顺序是固定的：先跑 `ll_app_do` 队列，再分发输入，然后调场景的 `update`，接着 `ll_anim_step(dt)`。到这里如果没有任何东西变脏（`g_dirty`、弹簧、屏幕脏页三者全空）就直接返回，一个字节都不推总线。要重画就清屏、调 `paint`、`ll_screen_flush()`。`ll_app_frame()` 在外面按 `ll_app_fps` 卡节拍，并把 dt 夹到 0.1 秒以内，防止串口断连之后回来一个巨大的 dt 把弹簧打飞。

I2C 是帧率瓶颈。全刷一帧 1024 字节，400kHz 下约 23ms，所以 30fps 已经接近上限。引擎按"只有变化的页才推"来省，静止时零流量，光标那一小块动也只推 1~2 页。

## 坐标空间

节点的 `outer` 是布局绝对坐标，不含动画位移；画布的裁剪矩形存的是屏幕绝对坐标。`ll_canvas_clip(c, r)` 的 `r` 是本地坐标，内部自己加 `c.ox/c.oy`。外面再手动减一次就会在页面滑动时错位，这个 bug 的现场是退场那页超出视口的行涂到了上一页身上。

同理，画自己的控件时原点已经推到节点左上角，矩形要传 `{0,0,w,h}`，把 `outer` 原样递进去会加两遍，整颗按钮飞出屏幕。

## 单色屏上没有"灰"

SSD1306 一个像素要么亮要么灭。层次只能靠形状和反色表达，按下就整块填满，字用背景色画出洞；禁用改用描边；次要信息换小字体。任何半透明的想法在这块屏上都会变成噪点抖动。

## 静态池

全程没有 malloc，池子在编译期就定下来了。`ll_conf.h` 里那几个 `LL_*_MAX` 就是额度，池子满了不会崩，会静默降级，`ll_node_new` 返回 NULL 那一行就不显示，`ll_value_init` 拿不到槽位那个动画直接跳到终值。

加界面之前先量一下。录帧程序末尾会打印节点池峰值（`ll_nodes_used()`），Gallery 那条链峰值 24，`LL_NODE_MAX` 给的是 96。`LlValue` 一个 28 字节，`LlNode` 因为带 16 个子指针槽位比它大得多，具体多少以 `sizeof` 为准。

## Tour 为什么是单线程

事件环按单生产者/单消费者设计，`g_head` 只由消费者写，`g_tail` 只由生产者写，中间留一格用来区分满和空。再加一个发事件的任务就变成两个生产者，需要锁。

SpringLab 和 Gallery 开的是 200Hz 的 FreeRTOS 任务读串口，因为那里只有一个生产者。Tour 自己就是一个生产者（按时间表往环里塞按键），所以它把读串口搬回了 `loop()`，整条链单线程。代价是消抖采样从 5ms 变成一帧 33ms，点按要 66ms 才生效，手感上钝一点点。想要跟手的连发，照 `Gallery.ino` 那样把 `ll_serial_keys_service()` 挪进任务里，同时把巡演也放进同一个任务。

顺便一提，arduino-esp32 里 `xTaskCreate` 的栈大小按字节算，和裸 FreeRTOS 的"字数"不一样。示例里给按键任务 4096 字节。

## extern "C"

Arduino 把 `.ino` 当 C++ 编，库里的 `.c` 按 C 编。C++ 会改写函数名，`ll_tour_reset` 变成 `_Z13ll_tour_resetv`，两边就成了两个符号。编译期一句警告都没有，链接才报 `undefined reference to 'll_tour_reset()'`。报错里那对括号就是改写过的标志，看到它先怀疑头文件少了 `extern "C"`。

引擎头统一由 `lilt_c.h` 包一层。三个 `demo_*.h` 是被 `.ino` 单独 include 的，落在包裹外面，所以各自带守卫。自己往库里加 `.c` 时，那个头也要照着加。

`sh host/build-esp32.sh` 末尾有一道 `nm -u` 检查，扫三个 `.ino` 的目标文件，出现 `_Z<数字>ll_` 就失败。不链接也能把这类问题挡在编译期。

## 不上板先看画面

引擎不碰硬件，所以在 PC 上编译同一份 `.c`，把 `ll_bus` 换成"写文件"的实现，就能录帧出图。

```sh
sh host/build.sh          # 编译 + 跑 101 项自检 + 录三段帧 + 出 GIF 和概览图
sh host/build-esp32.sh    # 用 Arduino 的 riscv32 工具链真交叉编译一遍，报占用
```

`host/build.sh` 产出 `out/tour.gif`、`out/tour-contact.png`、`out/gallery.gif`、`out/gallery-contact.png`、`out/spring-lab.gif`、`out/contact.png`。`*-contact.png` 是每 12 帧拼一张的概览图，一眼看完整个应用。想单独出图：

```sh
python host/render.py --src out/frames-tour.raw --gif out/t.gif --contact out/c.png --every 12
```

录下来的帧就是 SSD1306 的显存格式，页主序，一个字节竖着管 8 个像素，低位在上。`render.py` 按同样的方式解回 128×64，所以 PC 上看到的和板子上看到的必然一致。

自检覆盖的是能算清楚的部分：弹簧过冲比例对不对、Ease 会不会回弹、消抖要几次采样、队列满了丢谁、裁剪和跨页填充的字节对不对、方向导航会不会串到下一行、页面栈的节点回收干不干净、巡演字幕有没有超出 128 像素、连演两圈节点池回不回得去。画面对不对还是得看概览图，上面坐标空间那两个 bug 就是只有眼睛能发现的。

## 还没做的

文本输入框、键盘和编码器适配、后台任务与进度条、`Input` 的字符编辑，以及把按键换成真实 GPIO。
