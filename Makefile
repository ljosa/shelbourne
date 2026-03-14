IMAGE  = shelbourne-build
DEVICE ?= 192.168.2.42
DEST    = /mnt/nv
LIBDIR  = $(shell cd ../libshelbourne 2>/dev/null && pwd)
CC      = arm-linux-gnueabihf-gcc
CFLAGS  = -static -O2 -Wall

# shairplay library (AirPlay/RAOP receiver)
SP = shairplay/src/lib
SHAIRPLAY_SRC = \
	$(SP)/raop.c $(SP)/raop_rtp.c $(SP)/raop_buffer.c \
	$(SP)/rsakey.c $(SP)/rsapem.c $(SP)/aes_ctr.c \
	$(SP)/httpd.c $(SP)/http_parser.c $(SP)/http_request.c $(SP)/http_response.c \
	$(SP)/logger.c $(SP)/netutils.c $(SP)/sdp.c \
	$(SP)/base64.c $(SP)/digest.c $(SP)/pairing.c $(SP)/plist.c $(SP)/utils.c \
	$(SP)/fairplay_playfair.c \
	$(SP)/playfair/playfair.c $(SP)/playfair/hand_garble.c \
	$(SP)/playfair/modified_md5.c $(SP)/playfair/omg_hax.c \
	$(SP)/playfair/sap_hash.c \
	$(SP)/alac/alac.c \
	$(SP)/crypto/aes.c $(SP)/crypto/bigint.c $(SP)/crypto/hmac.c \
	$(SP)/crypto/md5.c $(SP)/crypto/rc4.c $(SP)/crypto/sha1.c \
	$(SP)/curve25519/curve25519-donna.c \
	$(SP)/ed25519/add_scalar.c $(SP)/ed25519/fe.c $(SP)/ed25519/ge.c \
	$(SP)/ed25519/key_exchange.c $(SP)/ed25519/keypair.c $(SP)/ed25519/sc.c \
	$(SP)/ed25519/seed.c $(SP)/ed25519/sha512.c $(SP)/ed25519/sign.c \
	$(SP)/ed25519/verify.c

# tinysvcmdns (mDNS/Bonjour service advertisement)
MDNS_SRC = tinysvcmdns/mdns.c tinysvcmdns/mdnsd.c

.PHONY: all clean deploy deploy-shepherd revert-shepherd docker

docker:
	@docker inspect $(IMAGE) >/dev/null 2>&1 || \
	  (cd $(LIBDIR) && docker build --platform linux/amd64 -t $(IMAGE) .)

all: docker
	@mkdir -p build
	docker run --rm \
	  -v "$(PWD):/work" \
	  -v "$(LIBDIR):/libshelbourne" \
	  --platform linux/amd64 $(IMAGE) \
	  sh -c '\
	    $(CC) $(CFLAGS) -I/libshelbourne \
	      -o /work/build/player \
	      /work/player.c /work/net.c /work/display.c /work/sun.c \
	      /work/airplay.c \
	      /libshelbourne/shelbourne.c \
	      -lm -lpthread && \
	    $(CC) $(CFLAGS) \
	      -I/work/shairplay/include/shairplay \
	      -I/work/shairplay/src/lib \
	      -I/work/tinysvcmdns \
	      -o /work/build/airplayd \
	      /work/airplayd.c \
	      $(addprefix /work/,$(SHAIRPLAY_SRC)) \
	      $(addprefix /work/,$(MDNS_SRC)) \
	      -lm -lpthread'
	@echo "Built: build/player build/airplayd"

deploy: all
	scp build/player root@$(DEVICE):$(DEST)/player.new
	scp build/airplayd root@$(DEVICE):$(DEST)/airplayd.new
	ssh root@$(DEVICE) "\
	  mv $(DEST)/player.new $(DEST)/player && \
	  mv $(DEST)/airplayd.new $(DEST)/airplayd && \
	  chmod +x $(DEST)/player $(DEST)/airplayd"
	@echo "Deployed to $(DEVICE):$(DEST)/{player,airplayd}"

deploy-shepherd: deploy
	ssh root@$(DEVICE) "mkdir -p /mnt/nv/shepherd"
	scp shepherd-override/Shepherd-spotty.xml root@$(DEVICE):/mnt/nv/shepherd/
	@echo "Shepherd override installed. Reboot to auto-start player + airplayd."

revert-shepherd:
	ssh root@$(DEVICE) "rm -rf /mnt/nv/shepherd && sync"
	@echo "Reverted to stock boot sequence. Reboot to restore stock software."

clean:
	rm -rf build
