# Service host tools (cast streamer, future emulator client, …)
ROOT := $(abspath ../..)

.PHONY: all cast client clean

all: cast client

cast:
	$(MAKE) -C cast

client:
	$(MAKE) -C client

clean:
	$(MAKE) -C cast clean
	$(MAKE) -C client clean
