#!/bin/sh
# build.sh — 在 PC 上编译并跑 A 阶段自检 + 录帧 + 出图
#
#   sh host/build.sh
#
# 产出：out/lab.exe（自检程序）、out/frames.raw（原始帧）、
#       out/spring-lab.gif（动画）、out/contact.png（概览图，一眼看完 420 帧）
set -e
cd "$(dirname "$0")/.."
mkdir -p out

gcc -std=gnu11 -Wall -Wextra -g -o out/lab \
    ll_anim.c ll_geom.c ll_gfx.c ll_screen.c ll_font.c ll_input.c ll_theme.c ll_app.c \
    ll_node.c ll_cursor.c ll_page.c \
    demo_spring.c demo_gallery.c demo_tour.c tf_notosans12pt.c tf_tinysz8pt7b.c \
    host/host_bus.c host/main.c -lm

./out/lab
python host/render.py --every 10
python host/render.py --src out/frames-gallery.raw --gif out/gallery.gif \
                      --contact out/gallery-contact.png --every 12
python host/render.py --src out/frames-tour.raw --gif out/tour.gif \
                      --contact out/tour-contact.png --every 12
