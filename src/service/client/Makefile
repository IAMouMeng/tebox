# Wails desktop launcher for Android instance management.
ROOT := $(abspath ../../..)
OUT := $(ROOT)/out/service
export PATH := $(shell go env GOPATH)/bin:$(PATH)
UNAME_S := $(shell uname -s)

.PHONY: all clean deps
all: $(OUT)/tebox-client

deps:
	cd frontend && npm install
	go mod tidy

$(OUT)/tebox-client: $(wildcard *.go) $(wildcard internal/tebox/*.go) $(shell find frontend/src -type f 2>/dev/null) frontend/index.html frontend/package.json wails.json
	@mkdir -p $(OUT)
	@test -d frontend/node_modules || (cd frontend && npm install)
	go mod tidy
	wails build -clean -nopackage -o tebox-client
	cp -f build/bin/tebox-client $(OUT)/tebox-client
	@chmod +x $(OUT)/tebox-client
ifeq ($(UNAME_S),Darwin)
	@# cp can leave a linker/adhoc signature that macOS AMFI SIGKILLs on launch.
	xattr -cr $(OUT)/tebox-client 2>/dev/null || true
	codesign --force --sign - $(OUT)/tebox-client
endif

clean:
	rm -rf build/bin frontend/dist frontend/node_modules
	rm -f $(OUT)/tebox-client
