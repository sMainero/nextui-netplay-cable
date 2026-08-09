.PHONY: dist shim cores app test clean help

PAK        := Netplay.pak
ARCHIVE    := dist/$(PAK).zip
STAGE      := dist/stage
PLATFORMS  := tg5040 tg5050 my282 my355 h700

# my355 and h700 are separate NextUI forks with their own pinned source trees,
# so they cannot be built from a bare toolchain image the way the others were -
# each needs its matching workspace mounted. build-platforms owns that pairing
# (image digest + source commit per platform); going through it is what makes
# a five-platform build reproducible rather than a guess about which aarch64
# tree happens to be close enough.
BUILDER    := $(HOME)/dev/sbcs/build-platforms

help:
	@echo "make shim    cross-build the shim for $(PLATFORMS) (needs docker)"
	@echo "make cores   build the patched gambatte + gpsp cores (needs docker)"
	@echo "make app     build the session-setup app (needs docker + NEXTUI)"
	@echo "make test    run the host test suites"
	@echo "make dist    stage and zip $(ARCHIVE)"
	@echo "make clean   remove dist/"

###########################################################

shim:
	@for p in $(PLATFORMS); do \
		echo "== shim $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd shim && make PLATFORM='$$p'"' >/dev/null || exit 1; \
	done
	@ls -la bin/*/netplay_shim.so | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

# Cores the pak ships in place of NextUI's.
#
#  gambatte - NextUI builds it without HAVE_NETWORK, so Game Link is compiled
#             out entirely and no configuration can reach it. Plus a serial
#             receive deadline (cores/patches/).
#  gpsp     - the two RFU fixes from the original pak: disconnect handling and
#             a packet queue deep enough for TCP's bursty delivery.
#
# Both are the same upstream sources NextUI uses; only the build differs.
GAMBATTE_SRC := dist/coresrc/gambatte
GPSP_SRC     := dist/coresrc/gpsp

# The `platform=` string is not our platform id - it selects a branch inside the
# core's own Makefile, and an unrecognised one silently falls through to some
# other target (snes9x picks *Windows*, and mgba stops with "no makefile
# found"). gpsp knows tg5040/tg5050/my282 and nothing else, so the two newer
# aarch64 targets borrow the tg5040 branch: it only sets CC/CXX/AR from
# CROSS_COMPILE plus -shared -fPIC, with no CPU tuning of its own, so the
# toolchain image decides the actual target. Determinism does not enter into it
# - both these cores are link-cable only, where each device runs its own
# console and no state is ever compared.
core_platform = $(if $(filter my355 h700,$(1)),tg5040,$(1))

define core_build
	@for p in $(PLATFORMS); do \
		echo "== $(1) $$p"; \
		mkdir -p dist/cores/$$p; \
		case $$p in my282) img=nextui-my282-toolchain:local ;; \
		            *) img=ghcr.io/loveretro/$$p-toolchain:latest ;; esac; \
		bp=$$p; case $$p in my355|h700) bp=tg5040 ;; esac; \
		docker run --rm -u "$$(id -u):$$(id -g)" -v "$$PWD":/w -w /w/$(2) \
			$$img sh -c "make $(3) platform=$$bp clean >/dev/null 2>&1; \
			make $(3) platform=$$bp -j4 >/dev/null 2>&1 && \
			cp $(1)_libretro.so /w/dist/cores/$$p/" || exit 1; \
	done
endef

cores: cores-gambatte cores-gpsp

cores-gambatte:
	@test -d "$(GAMBATTE_SRC)" || { echo "missing $(GAMBATTE_SRC)"; exit 1; }
	$(call core_build,gambatte,$(GAMBATTE_SRC),-f Makefile.libretro HAVE_NETWORK=1)
	@for p in $(PLATFORMS); do \
		printf "   %-7s %s  link=%s\n" "$$p" \
			"$$(strings -n 6 dist/cores/$$p/gambatte_libretro.so | grep -m1 '^v0\.5\.0')" \
			"$$(strings -n 6 dist/cores/$$p/gambatte_libretro.so | grep -c gambatte_gb_link_mode)"; \
	done
	@echo "   (link=0 means HAVE_NETWORK did not take - stock NextUI gambatte reads 0)"

cores-gpsp:
	@test -d "$(GPSP_SRC)" || { echo "missing $(GPSP_SRC)"; exit 1; }
	$(call core_build,gpsp,$(GPSP_SRC),)
	@for p in $(PLATFORMS); do \
		printf "   %s: rfu queue depth " "$$p"; \
		grep -c RFU_PKT_QUEUE_SIZE $(GPSP_SRC)/rfu.c; \
	done

# The UI needs GFX_/PAD_ from NextUI's common/api.c, which is compiled into
# minarch and unreachable from the shim - so the app carries its own copy.
NEXTUI ?= ../../NextUI

# NEXTUI=/opt/nextui-src is where build-platforms mounts the pinned tree for the
# platform being built - not a path on this machine.
app:
	@for p in $(PLATFORMS); do \
		echo "== app $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd app && make PLATFORM='$$p' NEXTUI=/opt/nextui-src"' >/dev/null || exit 1; \
	done
	@ls -la bin/*/netplay.elf | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

test:
	@./shim/test/run.sh
	@./shim/test/link.sh
	@./launcher/test.sh

###########################################################
# Test build.
#
# Built from an allowlist, not by excluding things from the repo. The repo
# carries ~16MB of patched per-NextUI-build binaries, a NextUI source overlay
# and the shim sources, none of which belong on the device - and an exclusion
# list silently ships whatever it forgets.
#
# Note this stages launcher/pak-launch.sh as launch.sh rather than the pak's own
# launch.sh. Both run bin/$PLATFORM/netplay.elf; pak-launch.sh is the one that
# sets SDCARD_PATH/SYSTEM_PATH/LOGS_PATH explicitly and creates the log dir, so
# it works when the frontend has not exported them.
#
# (An older comment here said netplay.elf was excluded from this build. That
# referred to a different, long-removed netplay.elf which installed patched
# system binaries - the thing the shim architecture replaced. The current app is
# shipped and is the pak's whole UI.)
###########################################################

dist: $(addprefix check-shim-,$(PLATFORMS))
	@rm -rf "$(STAGE)" "$(ARCHIVE)"
	@mkdir -p "$(STAGE)/$(PAK)/launcher" "$(STAGE)/$(PAK)/state" dist

	@cp pak.json "$(STAGE)/$(PAK)/"
	@cp launcher/pak-launch.sh "$(STAGE)/$(PAK)/launch.sh"
	@cp launcher/minarch.elf launcher/launch-stub.sh launcher/adhoc-join.sh launcher/wifi-watchdog.sh \
	    launcher/install-stubs.sh launcher/wrap-pak.sh launcher/bind-mount.sh launcher/pre-launch.sh \
	    "$(STAGE)/$(PAK)/launcher/"
	@cp launcher/session.conf.example "$(STAGE)/$(PAK)/"
	@for p in $(PLATFORMS); do \
		mkdir -p "$(STAGE)/$(PAK)/bin/$$p"; \
		cp bin/$$p/netplay_shim.so "$(STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay.elf "$(STAGE)/$(PAK)/bin/$$p/"; \
		for c in gambatte gpsp; do \
			if [ -f dist/cores/$$p/$${c}_libretro.so ]; then \
				mkdir -p "$(STAGE)/$(PAK)/cores/override/$$p"; \
				cp dist/cores/$$p/$${c}_libretro.so "$(STAGE)/$(PAK)/cores/override/$$p/"; \
			fi; \
		done; \
	done

	@chmod 755 "$(STAGE)/$(PAK)/launch.sh" "$(STAGE)/$(PAK)/launcher"/*
	@find "$(STAGE)" -name '.DS_Store' -delete

	@cd "$(STAGE)" && zip -q -r "../$(PAK).zip" "$(PAK)" -x '*/.*'
	@echo
	@echo "$(ARCHIVE)  ($$(du -h "$(ARCHIVE)" | cut -f1))"
	@unzip -l "$(ARCHIVE)" | tail -n +4 | head -n -2

check-shim-%:
	@test -f bin/$*/netplay_shim.so || { \
		echo "missing bin/$*/netplay_shim.so - run 'make shim' first"; exit 1; }

clean:
	rm -rf dist
