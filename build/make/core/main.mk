# AOSP analog of build/make/core/main.mk. `m` calls soong-lite directly;
# this file is for `make -f build/make/core/main.mk` from the tree root.
ifeq ($(TARGET_PRODUCT),)
  $(error Please lunch a combo first (source build/envsetup.sh && lunch))
endif
TOP ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
SOONG := $(TOP)/build/soong/soong.py

.DEFAULT_GOAL := all
.PHONY: all modules
all:
	python3 $(SOONG) --variant $(TARGET_PRODUCT)

modules:
	python3 $(SOONG) --variant $(TARGET_PRODUCT) --list

ifneq ($(filter-out all modules,$(MAKECMDGOALS)),)
$(MAKECMDGOALS):
	python3 $(SOONG) --variant $(TARGET_PRODUCT) $@
.PHONY: $(MAKECMDGOALS)
endif
