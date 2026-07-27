# Builds the small HOME Menu forwarder payload embedded in dolphin.nro.

DEVKITPRO ?= /opt/devkitpro

CC := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
TOOLS := $(DEVKITPRO)/tools/bin
OUT := ../../../Data/Sys/fwd

CFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft \
  -fPIE -O2 -DNDEBUG -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident \
  -ffile-prefix-map=$(CURDIR)=. -fmacro-prefix-map=$(CURDIR)=. \
  -I$(DEVKITPRO)/libnx/include -D__SWITCH__ -DVERSION='"0.3.21"'
LDFLAGS := -L$(DEVKITPRO)/libnx/lib -lnx -Wl,--gc-sections -Wl,--build-id=none \
  -Wl,--wrap,exit

all: $(OUT)/hbl.nso $(OUT)/hbl.npdm $(OUT)/default.nacp

hbl.elf: main.c trampoline.s
	$(CC) main.c trampoline.s -o $@ $(CFLAGS) $(LDFLAGS)

$(OUT)/hbl.nso: hbl.elf | $(OUT)
	$(TOOLS)/elf2nso $< $@

$(OUT)/hbl.npdm: hbl.json | $(OUT)
	$(TOOLS)/npdmtool $< $@

$(OUT)/default.nacp: Makefile | $(OUT)
	$(TOOLS)/nacptool --create Dolphin "Dolphin Emulator Project" 0.3.21 $@

$(OUT):
	mkdir -p $@

clean:
	rm -f hbl.elf
	rm -rf $(OUT)

.PHONY: all clean
