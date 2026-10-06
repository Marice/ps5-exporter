PS5_HOST ?= 192.168.68.125
PS5_PORT ?= 9021

TITLE := ps5-exporter
ELF   := $(TITLE).elf

# One source of truth for the version; also baked into ps5_exporter_build_info.
VERSION := 0.2.0

# The host tests build with the system compiler, so only the console targets
# need the SDK. Those depend on require-sdk and fail with a clear message.
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

CFLAGS := -Wall -Wextra -O2 -std=c11 -DEXPORTER_VERSION='"$(VERSION)"'
# -lkernel_sys is a link flag, not a compile flag: it provides the model name
# and the fan, CPU usage and sensor calls.
LDLIBS := -lkernel_sys

SRCS := src/main.c src/buf.c src/http.c src/metrics.c src/sensors.c src/shadowmount.c
HDRS := $(wildcard src/*.h)

HOST_CC ?= gcc
HOST_TEST_FLAGS := -Wall -Wextra -O1 -fsanitize=address,undefined -fno-sanitize-recover=all

all: $(ELF)

require-sdk:
	@test -n "$(PS5_PAYLOAD_SDK)" || { echo "PS5_PAYLOAD_SDK is undefined; export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk"; exit 1; }

$(ELF): require-sdk $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

# Host-side tests: append buffer, sensor range checks and the JSON parser,
# with the console calls faked. Sanitizers on. Run before every release.
test:
	@mkdir -p build
	@$(HOST_CC) $(HOST_TEST_FLAGS) -o build/test_exporter \
		tests/test_exporter.c src/buf.c src/sensors.c src/shadowmount.c src/metrics_stub.c
	@./build/test_exporter

# Development build with the /probe/<name> endpoint (undocumented API probing).
# Uploaded to its own folder so the payload autoloader cannot pick it up.
probe: $(TITLE)-probe.elf

$(TITLE)-probe.elf: require-sdk $(SRCS) src/probe.c $(HDRS)
	$(CC) $(CFLAGS) -DEXPORTER_PROBE -o $@ $(SRCS) src/probe.c $(LDLIBS)

upload-probe: $(TITLE)-probe.elf
	curl -sS -f --ftp-create-dirs -T $(TITLE)-probe.elf "ftp://$(PS5_HOST):1337/data/$(TITLE)-dev/$(TITLE)-probe.elf" \
		&& echo "probe uploaded to /data/$(TITLE)-dev/ (not autoloaded)"

# Send to a console running elfldr on PS5_PORT (not available with the
# localhost-only autoloader elfldr; use Payload Manager then).
deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

# Copy the ELF into the Payload Manager folder over FTP (etaHEN, port 1337).
upload: $(ELF)
	curl -sS -f --ftp-create-dirs -T $(ELF) "ftp://$(PS5_HOST):1337/data/pldmgr/payloads/$(TITLE)/$(ELF)" \
		&& echo "uploaded to /data/pldmgr/payloads/$(TITLE)/"

dist: $(ELF)
	mkdir -p dist && cp $(ELF) dist/

clean:
	rm -rf $(ELF) $(TITLE)-probe.elf *.o dist build

.PHONY: all require-sdk test deploy upload probe upload-probe dist clean
