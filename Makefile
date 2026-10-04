PS5_HOST ?= 192.168.68.125
PS5_PORT ?= 9021

TITLE := ps5-exporter
ELF   := $(TITLE).elf

ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK is undefined; export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# -lkernel_sys: payloads run with the privileges that libkernel_sys expects
# (the SDK hwinfo sample links it too); it provides the model name.
CFLAGS := -Wall -Wextra -O2 -std=c11 -lkernel_sys

$(ELF): src/main.c src/http.c src/metrics.c src/shadowmount.c src/*.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/http.c src/metrics.c src/shadowmount.c

# Send to a console running elfldr on PS5_PORT (not available with the
# localhost-only autoloader elfldr; use Payload Manager then).
test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

# Copy the ELF into the Payload Manager folder over FTP (etaHEN, port 1337).
upload: $(ELF)
	curl -sS -f --ftp-create-dirs -T $(ELF) "ftp://$(PS5_HOST):1337/data/pldmgr/payloads/$(TITLE)/$(ELF)" && echo "uploaded to /data/pldmgr/payloads/$(TITLE)/"

dist: $(ELF)
	mkdir -p dist && cp $(ELF) dist/

clean:
	rm -rf $(ELF) *.o dist

.PHONY: test upload dist clean
