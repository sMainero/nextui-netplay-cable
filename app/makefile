###########################################################
# Netplay.pak session-setup app
#
# Needs a NextUI checkout: the UI uses GFX_/PAD_ from common/api.c, which is
# compiled straight into minarch and so unreachable from the shim. We compile
# our own copy, exactly as the original pak's app did. The source and platform
# flags must come from the matching NextUI tree because platform.c is part of
# the app and is not a stable external SDK.
#
#   make PLATFORM=tg5040 NEXTUI=/path/to/NextUI
###########################################################

SUPPORTED_PLATFORMS = tg5040 tg5050 my282 my355 h700
TARGET  = netplay.elf
BROKER_TARGET = netplay-broker.elf
CABLE_TARGET  = netplay-cable.elf
NEXTUI ?= ../../../NextUI

# The cable link's pure layer is built and tested on the developer's machine:
# no cross compiler, no NextUI checkout, no device. `proto-test` and `clean`
# therefore skip every device check below, including the platform ABI include,
# exactly as shim/makefile does for its `native` target. MAKECMDGOALS is empty
# when `all` is reached as the default goal.
DEVICE_BUILD := 1
ifneq (,$(filter $(MAKECMDGOALS),proto-test clean))
DEVICE_BUILD :=
endif

ifeq (,$(PLATFORM))
PLATFORM = $(UNION_PLATFORM)
endif
ifeq (1,$(DEVICE_BUILD))
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE - run inside the NextUI toolchain container)
endif
endif

WS       = $(NEXTUI)/workspace
COMMON   = $(WS)/all/common
PLATDIR  = $(WS)/$(PLATFORM)/platform

# This is the platform ABI contract: CPU tuning, SDL/GLES include paths and the
# exact platform libraries. Reconstructing it here had already made tg5050 and
# my355 generic aarch64 builds and missed h700's GLES linking exception.
ifeq (1,$(DEVICE_BUILD))
include $(PLATDIR)/makefile.env
endif

SOURCE   = main.c netsetup.c \
           $(COMMON)/api.c $(COMMON)/utils.c $(COMMON)/config.c $(COMMON)/scaler.c \
           $(PLATDIR)/platform.c

# NEXTUI_SDK_PREFIX is set by build-platforms and holds a libmsettings built
# against the pinned source; the in-tree path is the fallback for a plain
# toolchain container. Both are offered so either environment works.
SDKPREFIX ?= $(NEXTUI_SDK_PREFIX)
INCDIR   = -I. -I$(COMMON) -I$(PLATDIR) -I$(WS)/$(PLATFORM)/libmsettings
ifneq (,$(SDKPREFIX))
INCDIR  += -I$(SDKPREFIX)/include
endif
CFLAGS  += $(OPT) $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS += -L$(WS)/$(PLATFORM)/libmsettings
ifneq (,$(SDKPREFIX))
LDFLAGS += -L$(SDKPREFIX)/lib
endif
LDFLAGS += -lmsettings -lsamplerate -lzip

# The detached broker links netsetup.c for discovery/compatibility logic but no
# frontend, audio, settings-library or archive APIs. Keep its idle process truly
# small instead of loading the setup app's entire SDL/GLES dependency graph.
BROKER_LDFLAGS = $(filter -L%,$(LDFLAGS)) -ldl

# The cable daemon links netsetup.c for the state path and the session grammar,
# exactly as the broker does, and nothing else from the app's dependency graph.
# -lpthread is for the one thread that reads the receive endpoint: in 4.9 the
# endpoint files have no poll(), so the receive direction cannot share the main
# loop (see usbnet.c). shim/makefile:70 is the tree's other -ldl -lpthread pair,
# and this is app/'s first pthread user.
CABLE_LDFLAGS  = $(filter -L%,$(LDFLAGS)) -ldl -lpthread

# The cable link's pure protocol layer, and the unit test that pins it.
#
# Native, and deliberately so: the descriptor blob, the framing codec and the
# status grammar are the parts of this feature a desktop can prove, and they are
# written as functions with no ambient state so that it can. Their warnings are
# stricter than the device build's, which is an ABI contract handed down by the
# platform makefile rather than a quality gate.
PROTO_TARGET  = cableproto-unit
PROTO_SOURCES = cableproto.c ../testing/$(PROTO_TARGET).c
PROTO_WARN    = -Wall -Wextra -Wno-unused-parameter
PROTO_PRODUCT = ../bin/native/$(PROTO_TARGET)

# Plan-local: `proto-test` is defined above `all`, so without this a bare `make`
# in this directory would build the native unit test instead of the device app -
# the first rule in the file is the default goal. Stated rather than left to
# rule order.
.DEFAULT_GOAL := all

proto-test: $(PROTO_PRODUCT)
	@$(PROTO_PRODUCT)

$(PROTO_PRODUCT): cableproto.c ../testing/$(PROTO_TARGET).c usbnet.h
	@mkdir -p ../bin/native
	$(CC) $(PROTO_SOURCES) -o $@ -I. -I../testing $(PROTO_WARN) -O2 -std=gnu99

PRODUCT = ../bin/$(PLATFORM)/$(TARGET)
BROKER_PRODUCT = ../bin/$(PLATFORM)/$(BROKER_TARGET)
CABLE_PRODUCT  = ../bin/$(PLATFORM)/$(CABLE_TARGET)

all:
	@mkdir -p ../bin/$(PLATFORM)
	$(CROSS_COMPILE)gcc $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
	@echo "built $(PRODUCT)"
	$(CROSS_COMPILE)gcc broker.c netsetup.c -o $(BROKER_PRODUCT) $(CFLAGS) $(BROKER_LDFLAGS)
	@echo "built $(BROKER_PRODUCT)"
	$(CROSS_COMPILE)gcc usbnet.c cableproto.c netsetup.c -o $(CABLE_PRODUCT) $(CFLAGS) $(CABLE_LDFLAGS)
	@echo "built $(CABLE_PRODUCT)"

clean:
	rm -f ../bin/*/$(TARGET) ../bin/*/$(BROKER_TARGET) ../bin/*/$(CABLE_TARGET) $(PROTO_PRODUCT)

.PHONY: all proto-test clean
