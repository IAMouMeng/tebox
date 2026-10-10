# tebox-cast against project FFmpeg only (thirdparty/ffmpeg).
# No Homebrew / system libav* — required for a self-contained package.

ROOT := $(abspath ../../..)
OUT := $(ROOT)/out/service
PREFIX_FFMPEG ?= $(ROOT)/out/cast-ffmpeg
HOST_ID := $(shell uname -s | tr A-Z a-z)-$(shell uname -m | sed 's/aarch64/arm64/')
PREBUILT_FFMPEG := $(ROOT)/prebuilts/host/$(HOST_ID)/ffmpeg

ifeq ($(wildcard $(PREBUILT_FFMPEG)/lib/pkgconfig/libavcodec.pc),)
  FFMPEG_HOME := $(PREFIX_FFMPEG)
else
  FFMPEG_HOME := $(PREBUILT_FFMPEG)
endif

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += -Iinclude -Isrc
LDFLAGS ?=

ifeq ($(wildcard $(FFMPEG_HOME)/lib/pkgconfig/libavcodec.pc),)
  $(error Bundled FFmpeg missing at $(FFMPEG_HOME). Run: ./scripts/build-ffmpeg-cast.sh)
endif

# Pin PKG_CONFIG_PATH for this recipe only so Homebrew cannot win.
PKG_CONFIG ?= pkg-config
PKG_ENV := PKG_CONFIG_PATH=$(FFMPEG_HOME)/lib/pkgconfig
AV_CFLAGS := $(shell $(PKG_ENV) $(PKG_CONFIG) --cflags libavcodec libavutil libswscale)
AV_LIBS := $(shell $(PKG_ENV) $(PKG_CONFIG) --libs libavcodec libavutil libswscale)

# Hard-pin include/lib dirs as well (pkg-config .pc may still mention brew deps).
AV_CFLAGS += -I$(FFMPEG_HOME)/include
AV_LIBS := -L$(FFMPEG_HOME)/lib -lavcodec -lswscale -lavutil

ifeq ($(shell uname -s),Darwin)
  RPATH := -Wl,-rpath,@loader_path/../ffmpeg/lib -Wl,-rpath,$(FFMPEG_HOME)/lib
else
  RPATH := -Wl,-rpath,'$$ORIGIN/../ffmpeg/lib' -Wl,-rpath,$(FFMPEG_HOME)/lib
endif

SRCS := \
	src/protocol.c \
	src/encoder.c \
	src/transport.c \
	src/capture.c \
	src/control.c \
	src/main.c

OBJS := $(SRCS:src/%.c=$(OUT)/%.o)
VIEW_OBJ := $(OUT)/cast_view.o

.PHONY: all clean demo

all: $(OUT)/tebox-cast $(OUT)/tebox-cast-view

$(OUT)/tebox-cast: $(OBJS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $^ $(AV_LIBS) $(RPATH) $(LDFLAGS)

$(OUT)/tebox-cast-view: $(VIEW_OBJ) $(OUT)/protocol.o
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: src/%.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(AV_CFLAGS) -c -o $@ $<

$(OUT)/cast_view.o: tools/cast_view.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

demo: $(OUT)/tebox-cast
	DYLD_LIBRARY_PATH=$(FFMPEG_HOME)/lib:$$DYLD_LIBRARY_PATH \
	  $(OUT)/tebox-cast --demo 320x240 --soft --out $(OUT)/demo.h264 --listen 127.0.0.1:9901

clean:
	rm -rf $(OUT)/tebox-cast $(OUT)/tebox-cast-view $(OUT)/*.o $(OUT)/demo.h264
