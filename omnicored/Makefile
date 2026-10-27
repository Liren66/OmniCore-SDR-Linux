# Builds the OmniCore no-OS SDK sources unchanged against the Linux BSP shims.
#   make SDK_SRC=<OmniCore-SDR-no-OS/src> CH32_DRV=<.../CH32H417_IP_v1_0/src> CC=arm-linux-gnueabihf-gcc O=<outdir>
SDK_SRC ?= ../../OmniCore-SDR-Firmware/OmniCore-SDR-Firmware.sdk/OmniCore-SDR-no-OS/src
CH32_DRV ?= ../../OmniCore-SDR-Firmware/OmniCore-SDR-Firmware.sdk/OmniCore-SDR-no-OS_bsp/ps7_cortexa9_0/libsrc/CH32H417_IP_v1_0/src
CC ?= arm-linux-gnueabihf-gcc
O ?= build

HERE := $(abspath .)
SDK_C := $(filter-out $(SDK_SRC)/main.c,$(wildcard $(SDK_SRC)/*.c)) \
         $(wildcard $(SDK_SRC)/console_commands/*.c) \
         $(wildcard $(SDK_SRC)/platform_xilinx/*.c) \
         $(CH32_DRV)/CH32H417_IP.c
OWN_C := $(HERE)/omnicored.c $(HERE)/hal.c $(HERE)/hal_tty.c

VERSION_STRING ?= OmniCore-SDR Linux (development build)
# Bitstream identity reported as system_info fpga=... (empty = SDK default "unknown").
FPGA_ID ?=
CPPFLAGS += -DOMNICORED_VERSION='"$(VERSION_STRING)"' -DOMNICORED_FPGA_ID='"$(FPGA_ID)"'
CPPFLAGS += -I$(HERE)/shim -I$(HERE) -I$(SDK_SRC) -I$(SDK_SRC)/console_commands \
            -I$(SDK_SRC)/platform_xilinx -I$(CH32_DRV) -D_GNU_SOURCE
CFLAGS ?= -O2 -g
CFLAGS += -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall -Wno-unused-function -Wno-format -MMD -MP
LDLIBS += -lm -pthread

obj = $(O)/$(subst /,_,$(subst $(abspath $(SDK_SRC))/,sdk/,$(subst $(abspath $(CH32_DRV))/,ch32/,$(subst $(HERE)/,,$(abspath $(1:.c=.o))))))
OBJS := $(foreach f,$(SDK_C) $(OWN_C),$(call obj,$(f)))

all: $(O)/omnicored

# Rebuild the daemon's own object when the version string changes.
$(O)/.version: FORCE | $(O)
	@echo '$(VERSION_STRING) $(FPGA_ID)' | cmp -s - $@ || echo '$(VERSION_STRING) $(FPGA_ID)' > $@
$(call obj,$(HERE)/omnicored.c): $(O)/.version

$(O)/omnicored: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

define compile_rule
$(call obj,$(1)): $(1) | $(O)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) -c -o $$@ $$<
endef
$(foreach f,$(SDK_C) $(OWN_C),$(eval $(call compile_rule,$(f))))

$(O):
	mkdir -p $@

clean:
	rm -rf $(O)

-include $(OBJS:.o=.d)
.PHONY: all clean FORCE
