.PHONY: dist shim test clean help

PAK        := Netplay.pak
ARCHIVE    := dist/$(PAK).zip
STAGE      := dist/stage
PLATFORMS  := tg5040 tg5050
TOOLCHAIN   = ghcr.io/loveretro/$(1)-toolchain:latest

help:
	@echo "make shim    cross-build the shim for $(PLATFORMS) (needs docker)"
	@echo "make test    run the host test suites"
	@echo "make dist    stage and zip $(ARCHIVE)"
	@echo "make clean   remove dist/"

###########################################################

shim:
	@for p in $(PLATFORMS); do \
		echo "== $$p"; \
		docker run --rm -u "$$(id -u):$$(id -g)" -v "$$PWD":/w -w /w/shim \
			$(call TOOLCHAIN,$$p) make PLATFORM=$$p || exit 1; \
	done

test:
	@./shim/test/run.sh
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
	@for p in $(PLATFORMS); do \
		mkdir -p "$(STAGE)/$(PAK)/bin/$$p"; \
		cp bin/$$p/netplay_shim.so "$(STAGE)/$(PAK)/bin/$$p/"; \
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
