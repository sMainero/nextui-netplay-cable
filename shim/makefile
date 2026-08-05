###########################################################
# netplay shim - libretro core wrapper
#
# Deliberately standalone: no NextUI workspace, no libmsettings, no SDL.
# It depends only on libc and the vendored libretro.h, which is the whole
# point - the shim tracks the libretro ABI, not a NextUI build.
#
#   make PLATFORM=tg5040      cross-build for the device
#   make native               host build, for smoke testing the forwarding
###########################################################

SUPPORTED_PLATFORMS = tg5040 tg5050

TARGET  = netplay_shim.so
INCDIR  = -Iinclude
WARN    = -Wall -Wextra -Wno-unused-parameter

# The device build is the point of this makefile; `native` is only ever asked
# for explicitly. Without this, `make PLATFORM=tg5040` would silently build for
# the host, because native happens to be defined first.
.DEFAULT_GOAL := all

###########################################################
# device build
###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

# Validate for the device build only - `native` and `clean` need none of this.
# MAKECMDGOALS is empty when `all` is reached as the default goal.
DEVICE_BUILD := 1
ifneq (,$(filter $(MAKECMDGOALS),native clean))
DEVICE_BUILD :=
endif

ifeq (1,$(DEVICE_BUILD))
ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. make PLATFORM=tg5040)
endif
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE - run inside the NextUI toolchain container)
endif
endif

# Matches workspace/tg5040/platform/makefile.env. No -flto: the shim is a thin
# forwarding layer and LTO buys nothing here while making symbol export
# behaviour harder to reason about.
CFLAGS  = -mcpu=cortex-a53 -O2 -fomit-frame-pointer -fPIC
CFLAGS += $(INCDIR) $(WARN) -std=gnu99
LDFLAGS = -shared -ldl

PRODUCT = ../bin/$(PLATFORM)/$(TARGET)

all:
	@mkdir -p ../bin/$(PLATFORM)
	$(CROSS_COMPILE)gcc shim.c -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
	@echo "built $(PRODUCT)"

###########################################################
# host build - not installable, just proves the C is sound
###########################################################

native:
	@mkdir -p ../bin/native
	$(CC) shim.c -o ../bin/native/$(TARGET) \
		-shared -fPIC $(INCDIR) $(WARN) -O2 -std=gnu99 -ldl

clean:
	rm -f ../bin/tg5040/$(TARGET) ../bin/tg5050/$(TARGET) ../bin/native/$(TARGET)

.PHONY: all native clean
