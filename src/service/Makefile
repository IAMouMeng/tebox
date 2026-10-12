# Service host tools (cast streamer, future emulator client, …)
ROOT := $(abspath ../..)

.PHONY: all cast client api clean

all: cast client api

cast:
	$(MAKE) -C cast

client:
	$(MAKE) -C client

api:
	$(MAKE) -C api

clean:
	$(MAKE) -C cast clean
	$(MAKE) -C client clean
	$(MAKE) -C api clean
