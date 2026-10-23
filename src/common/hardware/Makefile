# Compatibility: make -C src/common/hardware VARIANT=<product>
# Prefer: source build/envsetup.sh && lunch && m
.DEFAULT_GOAL := all
VARIANT ?= aosp_arm64
_gki_hw := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
TOP := $(abspath $(_gki_hw)/../../..)
SOONG := $(TOP)/build/soong/soong.py

.PHONY: all modules
all:
	python3 $(SOONG) --variant $(VARIANT)

modules:
	python3 $(SOONG) --variant $(VARIANT) --list

ifneq ($(filter-out all modules,$(MAKECMDGOALS)),)
$(MAKECMDGOALS):
	python3 $(SOONG) --variant $(VARIANT) $@
.PHONY: $(MAKECMDGOALS)
endif
