DEVICE ?= 192.168.2.42
DEST = /mnt/nv
LIBDIR = $(shell cd ../libshelbourne 2>/dev/null && pwd)
CC = arm-linux-gnueabihf-gcc
CFLAGS = -static -O2 -Wall

.PHONY: all clean deploy deploy-shepherd revert-shepherd

all:
	@mkdir -p build
	docker run --rm \
	  -v "$(PWD):/work" \
	  -v "$(LIBDIR):/libshelbourne" \
	  --platform linux/amd64 ubuntu:22.04 \
	  bash -c "apt-get update -qq && \
	           apt-get install -y -qq gcc-arm-linux-gnueabihf >/dev/null 2>&1 && \
	           $(CC) $(CFLAGS) -I/libshelbourne \
	             -o /work/build/player \
	             /work/player.c /libshelbourne/shelbourne.c -lm"
	@echo "Built: build/player"

deploy: all
	scp build/player root@$(DEVICE):$(DEST)/player
	ssh root@$(DEVICE) "chmod +x $(DEST)/player"
	@echo "Deployed to $(DEVICE):$(DEST)/player"

deploy-shepherd: deploy
	ssh root@$(DEVICE) "mkdir -p /mnt/nv/shepherd"
	scp shepherd-override/Shepherd-spotty.xml root@$(DEVICE):/mnt/nv/shepherd/
	@echo "Shepherd override installed. Reboot to auto-start player."

revert-shepherd:
	ssh root@$(DEVICE) "rm -rf /mnt/nv/shepherd"
	@echo "Reverted to stock boot sequence."

clean:
	rm -rf build
