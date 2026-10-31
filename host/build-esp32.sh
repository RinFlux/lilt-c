#!/bin/sh
# build-esp32.sh — 用 Arduino ESP32 自带的交叉编译器，把引擎 + 板子端代码真编译一遍
#
#   sh host/build-esp32.sh
#
# 不打开 IDE 也能确认"这份代码在 ESP32-C3 上编得过"，并且报出 flash / RAM 占用。
# 路径从 %LOCALAPPDATA%/Arduino15 自动找（Arduino IDE 装过 esp32 板包就有）。
set -e
cd "$(dirname "$0")/.."

BASE="$LOCALAPPDATA/Arduino15/packages"
[ -d "$BASE" ] || BASE="/c/Users/$USERNAME/AppData/Local/Arduino15/packages"

CORE=$(ls -d "$BASE"/esp32/hardware/esp32/*/ 2>/dev/null | sort -V | tail -1)
LIBS=$(ls -d "$BASE"/esp32/tools/esp32c3-libs/*/ 2>/dev/null | sort -V | tail -1)
BIN=$(ls -d "$BASE"/esp32/tools/esp-rv32/*/ 2>/dev/null | sort -V | tail -1)

if [ -z "$CORE" ] || [ -z "$LIBS" ] || [ -z "$BIN" ]; then
    echo "没找到 ESP32 核心 / C3 预编译库 / riscv32 工具链：先在 Arduino IDE 里装 esp32 板包" >&2
    exit 1
fi
echo "core      = $CORE"
echo "c3-libs   = $LIBS"
echo "toolchain = $BIN"

# 头文件路径写在板包的 flags/ 里，用 -iwithprefixbefore 表示"相对 include 根"，
# 这里把它展开成一条条 -I，连同宏定义一起塞进 gcc 的响应文件（命令行太长会被系统截断）。
CORE="$CORE" LIBS="$LIBS" python - <<'PY'
import io, os
core = os.environ["CORE"].replace("\\", "/").rstrip("/") + "/"
libs = os.environ["LIBS"].replace("\\", "/").rstrip("/") + "/"
inc  = io.open(libs + "flags/includes", encoding="utf-8").read().split()
defs = io.open(libs + "flags/defines", encoding="utf-8").read().split()

out = []
i = 0
while i < len(inc):
    if inc[i] == "-iwithprefixbefore":
        out.append("-I" + libs + "include/" + inc[i + 1])
        i += 2
    else:
        i += 1
out += [
    "-I" + core + "cores/esp32",
    "-I" + core + "variants/esp32c3",
    "-I" + core + "libraries/Wire/src",
    "-I" + core + "libraries/FS/src",
    "-I" + libs + "qio_qspi/include",     # sdkconfig.h 在这里
    "-I.",
]
out += defs + [
    "-DARDUINO=10819",
    "-DARDUINO_ESP32C3_DEV",
    "-DARDUINO_USB_MODE=1",
    "-DARDUINO_USB_CDC_ON_BOOT=1",
]
io.open("out/esp32.rsp", "w", encoding="utf-8").write(" ".join(out))
PY

CC="$BIN""bin/riscv32-esp-elf-gcc"
CXX="$BIN""bin/riscv32-esp-elf-g++"
SZ="$BIN""bin/riscv32-esp-elf-size"
NM="$BIN""bin/riscv32-esp-elf-nm"
MARCH="-march=rv32imc_zicsr_zifencei -g -Wall -Wextra -Wno-unused-parameter"
RSP="@out/esp32.rsp"
rm -rf out/esp32
mkdir -p out/esp32

for f in ll_anim ll_geom ll_gfx ll_screen ll_font ll_input ll_theme ll_app \
         ll_node ll_cursor ll_page \
         demo_spring demo_gallery demo_tour tf_notosans12pt tf_tinysz8pt7b; do
    "$CC" $MARCH -std=gnu17 $RSP -c "$f.c" -o "out/esp32/$f.o"
done
"$CXX" $MARCH -fno-rtti -std=gnu++2b $RSP \
    -c ll_board.cpp -o out/esp32/ll_board.o
# .ino 直接告诉编译器"按 C++ 读"，不在库里留下 .cpp 副本。
# 原因：Arduino 编译库时会递归收库里所有 .c/.cpp（只有 examples/ 和 . 开头的目录被跳过），
# out/ 下留下 .cpp 副本就会被当成库源码编进去，setup()/loop() 直接重复定义。
"$CXX" $MARCH -fno-rtti -std=gnu++2b $RSP \
    -x c++ -c examples/SpringLab/SpringLab.ino -o out/esp32/SpringLab.o
"$CXX" $MARCH -fno-rtti -std=gnu++2b $RSP \
    -x c++ -c examples/Gallery/Gallery.ino -o out/esp32/Gallery.o
"$CXX" $MARCH -fno-rtti -std=gnu++2b $RSP \
    -x c++ -c examples/Tour/Tour.ino -o out/esp32/Tour.o

echo
echo "== 全部编过（-Wall -Wextra 无警告）=="

# 只"编得过"还不够：.ino 按 C++ 编、.c 按 C 编，链接名对不上时编译器一声不响，
# 要到 IDE 链接那一步才炸（undefined reference to 'll_tour_reset()'，带括号的就是
# 被 C++ 改写过的名字）。这里直接查目标文件里的未定义引用，把链接期的错提前到编译期。
echo "== 链接名检查（.ino 引的引擎名字必须是 C 名字）=="
bad=0
for o in SpringLab Gallery Tour; do
    mangled=$("$NM" -u "out/esp32/$o.o" | grep -E 'U _Z[0-9]+ll_' || true)
    if [ -n "$mangled" ]; then
        echo "   x $o.o 引用了被 C++ 改写过的引擎名（对应头文件少 extern \"C\"）："
        echo "$mangled" | sed 's/^/       /'
        bad=1
    fi
done
cnames=$("$NM" -u out/esp32/SpringLab.o out/esp32/Gallery.o out/esp32/Tour.o \
         | grep -E 'U ll_' | sort -u | wc -l)
if [ "$bad" -eq 0 ]; then
    echo "   ok 三个 .ino 共引用 $cnames 个 C 链接名，没有一个被改写"
    # 现在实际是 16 个（ll_screen_init / ll_pages_boot / ll_app_frame / ll_tour_* ...）。
    # 低于 12 说明某个 .ino 根本没引到引擎，那这道检查就成了空转。
    if [ "$cnames" -lt 12 ]; then
        echo "   ! 引用数少得可疑，检查逻辑可能没真跑到"
        exit 1
    fi
else
    exit 1
fi

echo
echo "   下面是三个 demo + 三个 .ino + 两套字体的总和，不含 Arduino 核心和 IDF 库；"
echo "   真正烧进板子的固件只取其中一条链，比这个数小。"
"$SZ" -t out/esp32/*.o | tail -1
