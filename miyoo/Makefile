# Cross-compiles inside the Onion toolchain image and stages an Onion app in
# dist/App/lgtm, ready to copy to /mnt/SDCARD/App/lgtm.
IMAGE := aemiii91/miyoomini-toolchain:latest
CC := /opt/miyoomini-toolchain/bin/arm-linux-gnueabihf-gcc
CFLAGS := -O2 -Wall -Wextra -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
LIBS := -lSDL -lSDL_ttf -lSDL_mixer -lpthread

APP := dist/App/lgtm

# HTTPS on the device is a static curl (stunnel/static-curl, armv7 musl) and
# Mozilla's CA bundle. Both are downloaded at build time and checked, rather
# than committed.
CURL_VERSION := 8.22.0
CURL_SHA256 := 8c6450bbd9d8ff4f8a82144b49a47aa3a08a887d11552306f21705c104fa35d5
SHA256 := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo shasum -a 256)

all: $(APP)/lgtm

vendor/curl:
	curl -fsSL "https://github.com/stunnel/static-curl/releases/download/$(CURL_VERSION)/curl-linux-armv7-musl-$(CURL_VERSION).tar.xz" \
		| tar -xJ -C vendor curl
	echo "$(CURL_SHA256)  vendor/curl" | $(SHA256) -c -

vendor/cacert.pem:
	curl -fsSLo $@ https://curl.se/ca/cacert.pem
	curl -fsSL https://curl.se/ca/cacert.pem.sha256 | sed 's#cacert.pem#vendor/cacert.pem#' | $(SHA256) -c -

build/lgtm: src/main.c
	mkdir -p build
	docker run --rm --platform linux/amd64 -v "$(CURDIR)":/src -w /src $(IMAGE) \
		$(CC) $(CFLAGS) -o $@ $< $(LIBS)

$(APP)/lgtm: build/lgtm app/* vendor/fonts/* vendor/curl vendor/cacert.pem
	mkdir -p $(APP)/bin $(APP)/fonts
	cp build/lgtm $(APP)/lgtm
	cp app/launch.sh app/config.json app/icon.png app/chime.wav app/config.txt.example $(APP)/
	cp vendor/fonts/* $(APP)/fonts/
	cp vendor/curl vendor/cacert.pem $(APP)/bin/
	[ -f app/config.txt ] && cp app/config.txt $(APP)/ || true

# The release zip: extract onto the SD card root. Never includes config.txt.
package: all
	rm -f dist/lgtm-miyoo.zip
	cd dist && zip -qr lgtm-miyoo.zip App -x 'App/lgtm/config.txt'

# make deploy DEVICE=onion@<miyoo-ip>
SSH_OPTS ?=
deploy: all
	@test -n "$(DEVICE)" || (echo "set DEVICE=onion@<miyoo-ip>" && false)
	# Onion's dropbear has no scp, so ship a tarball over the ssh pipe.
	COPYFILE_DISABLE=1 tar -C $(APP) -cf - . | ssh $(SSH_OPTS) $(DEVICE) 'mkdir -p /mnt/SDCARD/App/lgtm && tar -C /mnt/SDCARD/App/lgtm -xf -'

clean:
	rm -rf build dist

.PHONY: all package deploy clean
