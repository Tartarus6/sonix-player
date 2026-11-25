# Makefile for Sonix Player standalone unit / property tests
#
# No LVGL context, no SDL, no cross-compiler needed.
#
#   make pod_layout_test   — build pod_layout tests
#   make home_theme_test   — build home_theme tests
#   make run               — build + run all tests
#   make run_pod           — build + run pod_layout tests only
#   make run_theme         — build + run home_theme tests only

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wno-unused-parameter \
          -g -O0 \
          -I. \
          -Istub_lvgl \
          -Istubs \
          -I..

# The modules under test live one directory up.
SRC_DIR      = ../src/gui/shell

SRCS_POD     = pod_layout_test.c $(SRC_DIR)/pod_layout.c
SRCS_THEME   = home_theme_test.c

pod_layout_test: $(SRCS_POD) stubs/src/system/core/config.h stubs/src/system/core/lang.h \
                 stub_lvgl/lvgl/lvgl.h
	$(CC) $(CFLAGS) -o $@ pod_layout_test.c $(SRC_DIR)/pod_layout.c

# home_theme_test is self-contained: stubs and the two functions live inside the .c file.
home_theme_test: $(SRCS_THEME)
	$(CC) $(CFLAGS) -o $@ home_theme_test.c

run: pod_layout_test home_theme_test
	./pod_layout_test
	./home_theme_test

run_pod: pod_layout_test
	./pod_layout_test

run_theme: home_theme_test
	./home_theme_test

clean:
	rm -f pod_layout_test home_theme_test

.PHONY: run run_pod run_theme clean
