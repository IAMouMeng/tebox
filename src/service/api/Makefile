ROOT := $(abspath ../../..)
OUT := $(ROOT)/out/service

.PHONY: all clean
all: $(OUT)/tebox-api

$(OUT)/tebox-api: $(wildcard cmd/tebox-api/*.go) $(wildcard internal/tebox/*.go) $(wildcard api/v1/*.go)
	@mkdir -p $(OUT)
	go build -o $@ ./cmd/tebox-api
	@chmod +x $@

clean:
	rm -f $(OUT)/tebox-api
