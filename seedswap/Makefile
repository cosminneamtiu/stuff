# SeedSwap NRO for kaiHEN (Seedsow game switcher). Build in WSL's ounce-devkit
# image (as WSL root, which is in the docker group):
#   docker run --rm -v "$PWD:/work" -w /work ounce-devkit:latest make
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif

TARGET := seedswap
VERSION := 0.12.1
BUILD := build
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
include $(DEVKITPRO)/libounce/switch2_cpu.mk

ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-format-truncation -ffunction-sections -fdata-sections \
    -D__SWITCH__ -D__SWITCH2__ \
    -I$(DEVKITPRO)/portlibs/switch2/include -I$(DEVKITPRO)/libounce/include -I$(DEVKITPRO)/portlibs/switch/include
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libounce/switch.specs -Wl,--gc-sections
LIBS := -L$(DEVKITPRO)/portlibs/switch2/lib -L$(DEVKITPRO)/libounce/lib -L$(DEVKITPRO)/portlibs/switch/lib -lzstd -lnx -lm

all: $(TARGET).nro

$(BUILD):
	mkdir -p $@

SOURCES := main ui model util gfx nsp plat_switch font_data compat_game crypto dlcpkg dumpcheck users aoc dlc_game ncz pkg stage fixes compat_words convert hosts
COMPAT := a64 nsofile cmpt compat
OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SOURCES))) $(addprefix $(BUILD)/compat_,$(addsuffix .o,$(COMPAT))) \
    $(BUILD)/stb_impl.o
HEADERS := $(wildcard source/*.h) $(wildcard source/compat/*.h)

$(BUILD)/%.o: source/%.c $(HEADERS) | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

# the patch engine (source/compat): plain C, the same files the PC tests build
$(BUILD)/compat_%.o: source/compat/%.c $(HEADERS) | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

# third-party decoder: no -Werror
$(BUILD)/stb_impl.o: source/stb_impl.c source/stb_cfg.h | $(BUILD)
	$(PREFIX)gcc $(filter-out -Werror,$(CFLAGS)) -w -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(PREFIX)gcc $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/$(TARGET).nacp: | $(BUILD)
	$(DEVKITPRO)/tools/bin/nacptool --create "SeedSwap" "local" "$(VERSION)" $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp assets/icon.jpg
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ --nacp=$(BUILD)/$(TARGET).nacp --icon=assets/icon.jpg

clean:
	rm -rf $(BUILD) $(TARGET).nro

.PHONY: all clean
