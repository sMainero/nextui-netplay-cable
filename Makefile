.PHONY: dist shim cores app test clean help

PAK        := Netplay.pak
ARCHIVE    := dist/$(PAK).zip
STAGE      := dist/stage
PLATFORMS  := tg5040 tg5050 my282

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
		echo "== $$p"; \
		case $$p in my282) img=nextui-my282-toolchain:local ;; \
		            *) img=ghcr.io/loveretro/$$p-toolchain:latest ;; esac; \
		docker run --rm -u "$$(id -u):$$(id -g)" -v "$$PWD":/w -w /w/shim \
			$$img make PLATFORM=$$p || exit 1; \
	done

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

define core_build
	@for p in $(PLATFORMS); do \
		echo "== $(1) $$p"; \
		mkdir -p dist/cores/$$p; \
		case $$p in my282) img=nextui-my282-toolchain:local ;; \
		            *) img=ghcr.io/loveretro/$$p-toolchain:latest ;; esac; \
		docker run --rm -u "$$(id -u):$$(id -g)" -v "$$PWD":/w -w /w/$(2) \
			$$img sh -c "make $(3) platform=$$p clean >/dev/null 2>&1; \
			make $(3) platform=$$p -j4 >/dev/null 2>&1 && \
			cp $(1)_libretro.so /w/dist/cores/$$p/" || exit 1; \
	done
endef

cores: cores-gambatte cores-gpsp

cores-gambatte:
	@test -d "$(GAMBATTE_SRC)" || { echo "missing $(GAMBATTE_SRC)"; exit 1; }
	$(call core_build,gambatte,$(GAMBATTE_SRC),-f Makefile.libretro HAVE_NETWORK=1)
	@for p in $(PLATFORMS); do \
		echo "   $$p: $$(strings -n 6 dist/cores/$$p/gambatte_libretro.so | grep -m1 '^v0\.5\.0')"; \
	done

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

app:
	@test -d "$(NEXTUI)/workspace" || { echo "set NEXTUI=/path/to/NextUI"; exit 1; }
	@for p in $(PLATFORMS); do \
		echo "== app $$p"; \
		case $$p in my282) img=nextui-my282-toolchain:local ;; \
		            *) img=ghcr.io/loveretro/$$p-toolchain:latest ;; esac; \
		docker run --rm -v "$$PWD":/w -v "$$(cd $(NEXTUI) && pwd)":/nextui:ro -w /w/app \
			$$img sh -c "make PLATFORM=$$p NEXTUI=/nextui >/dev/null && \
			chown $$(id -u):$$(id -g) ../bin/$$p/netplay.elf" || exit 1; \
	done

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
# Note this stages launcher/pak-launch.sh as launch.sh rather than the pak's
# own launch.sh, which runs netplay.elf: that app installs patched system
# binaries, which is precisely what the shim architecture replaces. netplay.elf
# is deliberately not in this build.
###########################################################

dist: $(addprefix check-shim-,$(PLATFORMS))
	@rm -rf "$(STAGE)" "$(ARCHIVE)"
	@mkdir -p "$(STAGE)/$(PAK)/launcher" "$(STAGE)/$(PAK)/state" dist

	@cp pak.json "$(STAGE)/$(PAK)/"
	@cp launcher/pak-launch.sh "$(STAGE)/$(PAK)/launch.sh"
	@cp launcher/minarch.elf launcher/launch-stub.sh \
	    launcher/install-stubs.sh launcher/wrap-pak.sh \
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
