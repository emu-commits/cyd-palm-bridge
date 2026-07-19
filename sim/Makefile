# sim/Makefile -- build the Palm UI simulator (docs/SIMULATOR_PLAN.md).
#
#   make -C sim host    native headless sim (scripted input, PPM screenshots)
#   make -C sim wasm    browser build via emcc -> build/web/ (index.html+js+wasm)
#   make -C sim smoke   build host + run the scripted smoke test (CI gate)
#   make -C sim clean
#
# LVGL v9.2.2 source is fetched (shallow clone) into sim/lvgl on first build
# (gitignored -- not vendored). The native `host` target needs /sdcard to exist
# (the firmware data layer's SD root): `mkdir /sdcard` locally, or on a CI
# runner `sudo mkdir -p /sdcard && sudo chmod 777 /sdcard`.

LVGL_TAG := v9.2.2
LVGL_URL := https://github.com/lvgl/lvgl.git

CC    ?= cc
EMCC  ?= emcc

FW := ../firmware/main
BR := ../bridge

# sim/include FIRST: shields the build from a real (gitignored) secrets.h and
# provides the esp_log/nvs/esp_timer shims that let real firmware sources compile.
INC    := -I. -Iinclude -Ilvgl -I$(FW) -I$(BR)
# UI_DEVTOOLS: keep the "Add test events" dev menu item in sim builds (used to
# mass-generate records against the heap/pool ceilings). Release firmware drops it.
CFLAGS := -O2 -g -Wall -DLV_CONF_INCLUDE_SIMPLE -DUI_DEVTOOLS $(INC)

# the REAL firmware UI + data layer + portable modules, unchanged
APP_SRCS := \
    $(FW)/ui.c $(FW)/data.c $(FW)/graffiti.c $(FW)/appcfg.c $(FW)/clock.c \
    $(FW)/lv_font_palm.c $(FW)/lv_font_palm_bold.c $(FW)/lv_font_kana.c $(FW)/palm_icons.c \
    $(FW)/kana_data.c $(FW)/kana_strokes.c $(FW)/kana_write.c \
    $(BR)/pdb.c $(BR)/datebook.c $(BR)/address.c $(BR)/todo.c $(BR)/appinfo.c \
    $(BR)/ical.c $(BR)/vcard.c $(BR)/tz.c $(BR)/charset.c \
    $(BR)/find.c $(BR)/calc.c $(BR)/config.c $(BR)/rss.c $(BR)/news.c $(BR)/feeds.c \
    stubs/hotsync_stub.c stubs/power_stub.c \
    sim_port.c heap_budget.c

# route every libc allocation through the device-like heap budget (sim_heap.h)
WRAP_LD := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

LVGL_SRCS := $(shell find lvgl/src -name '*.c' 2>/dev/null)
LVGL_OBJS := $(patsubst lvgl/src/%.c,build/lvgl/%.o,$(LVGL_SRCS))

.PHONY: host wasm smoke graf clean lvgl-src

# LVGL_SRCS is evaluated when make parses this file, so on a clean checkout the
# source must be fetched BEFORE the build graph is used: the entry points fetch,
# then recurse so the sub-make sees the cloned tree.
host: lvgl-src
	@$(MAKE) --no-print-directory build/sim_host
wasm: lvgl-src
	@$(MAKE) --no-print-directory build/web/index.html

# ---- LVGL source fetch (pinned) ----
lvgl-src:
	@test -d lvgl/src || git clone --depth 1 --branch $(LVGL_TAG) $(LVGL_URL) lvgl

lvgl/src:
	git clone --depth 1 --branch $(LVGL_TAG) $(LVGL_URL) lvgl

# ---- native: LVGL -> static lib (built once), app sources compiled per link ----
build/lvgl/%.o: lvgl/src/%.c lv_conf.h | lvgl/src
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

build/liblvgl.a: lvgl/src $(LVGL_OBJS)
	ar rcs $@ $(LVGL_OBJS)

build/sim_host: build/liblvgl.a $(APP_SRCS) host_main.c sim_port.h sim_heap.h lv_conf.h
	$(CC) $(CFLAGS) $(APP_SRCS) host_main.c build/liblvgl.a -o $@ -lm $(WRAP_LD)

# ---- smoke: the CI gate (scripted run must exit 0 and produce shots) ----
smoke: host
	rm -rf build/shots && mkdir -p build/shots
	./build/sim_host build/shots < tests/smoke.txt
	@ls build/shots/*.ppm >/dev/null
	@echo "sim smoke: OK"

# ---- graf: offline Graffiti recognizer accuracy gate (no LVGL/display needed).
# Synthesizes noisy strokes from the real templates and asserts recognition
# accuracy; a template edit that regresses recognition fails here. ----
graf: build/graf_test
	./build/graf_test

build/graf_test: tests/graf_test.c $(FW)/graffiti.c $(FW)/graffiti.h
	@mkdir -p build
	$(CC) -O2 -Wall -Iinclude -I$(FW) -DGRAF_TEST_HOOKS \
	    tests/graf_test.c $(FW)/graffiti.c -o $@ -lm

# ---- wasm: everything compiled by emcc in one shot (CI build; no local emcc needed).
# 32-bit wasm => the TRUE 24 KB device pool applies (see lv_conf.h). main() boots
# the Palm at module init; JS drives frames and blits the exported framebuffer. ----
WASM_CFLAGS := $(filter-out -g,$(CFLAGS)) -g0

build/web/index.html: lvgl/src $(APP_SRCS) wasm_main.c web/index.html lv_conf.h sim_heap.h
	mkdir -p build/web
	$(EMCC) $(WASM_CFLAGS) \
	    $(LVGL_SRCS) $(APP_SRCS) wasm_main.c \
	    -o build/web/palm.js \
	    -lidbfs.js $(WRAP_LD) \
	    -sEXPORTED_FUNCTIONS=_main,_sim_boot,_sim_frame,_sim_pointer,_sim_fb,_sim_scrub_config,_sim_heap_used,_sim_heap_freebytes \
	    -sEXPORTED_RUNTIME_METHODS=HEAPU8,FS \
	    -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=0 \
	    -sMODULARIZE=1 -sEXPORT_NAME=PalmSim
	cp web/index.html build/web/index.html
	@echo "wasm: build/web/ ready (index.html + palm.js + palm.wasm)"

clean:
	rm -rf build
