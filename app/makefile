###########################################################
# Netplay.pak session-setup app
#
# Needs a NextUI checkout: the UI uses GFX_/PAD_ from common/api.c, which is
# compiled straight into minarch and so unreachable from the shim. We compile
# our own copy, exactly as the original pak's app did - at runtime this depends
# only on libmsettings and SDL, not on a matching NextUI build.
#
#   make PLATFORM=tg5040 NEXTUI=/path/to/NextUI
###########################################################

SUPPORTED_PLATFORMS = tg5040 tg5050 my282
TARGET  = netplay.elf
NEXTUI ?= ../../../NextUI

ifeq (,$(PLATFORM))
PLATFORM = $(UNION_PLATFORM)
endif
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE - run inside the NextUI toolchain container)
endif

WS       = $(NEXTUI)/workspace
COMMON   = $(WS)/all/common
PLATDIR  = $(WS)/$(PLATFORM)/platform

SOURCE   = main.c netsetup.c \
           $(COMMON)/api.c $(COMMON)/utils.c $(COMMON)/config.c $(COMMON)/scaler.c \
           $(PLATDIR)/platform.c

INCDIR   = -I. -I$(COMMON) -I$(PLATDIR) -I$(WS)/$(PLATFORM)/libmsettings
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99 -O2
CFLAGS  += -DUSE_SDL2 -DUSE_GLES -DGL_GLEXT_PROTOTYPES
CFLAGS  += -I$(PREFIX)/include -I$(PREFIX_LOCAL)/include
LDFLAGS += -L$(PREFIX)/lib -L$(PREFIX_LOCAL)/lib -L$(PREFIX)/lib/$(CROSS_TRIPLE)
LDFLAGS += -L$(WS)/$(PLATFORM)/libmsettings
LDFLAGS += $(shell pkg-config --libs sdl2 glesv2 2>/dev/null || echo "-lSDL2 -lGLESv2")
LDFLAGS += -lSDL2_image -lSDL2_ttf -lmsettings -lsamplerate -lzip -lm -lpthread -ldl -lz -lasound

PRODUCT = ../bin/$(PLATFORM)/$(TARGET)

all:
	@mkdir -p ../bin/$(PLATFORM)
	$(CROSS_COMPILE)gcc $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
	@echo "built $(PRODUCT)"

clean:
	rm -f ../bin/*/$(TARGET)

.PHONY: all clean
