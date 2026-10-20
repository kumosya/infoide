CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Werror
SDK ?= $(if $(OSINFO_SDK),$(OSINFO_SDK),resources/sdk)
DISK ?= $(if $(OSINFO_DISK),$(OSINFO_DISK),resources/osinfo.img)
WITH_CDROM ?= 0
ISO ?= $(if $(OSINFO_ISO),$(OSINFO_ISO),$(if $(filter 1,$(WITH_CDROM)),resources/osinfo.iso))
INPKG_SOURCE ?= ../inpkg
PACKAGE_SOURCES := $(wildcard $(INPKG_SOURCE)/src/common/*.cc) $(INPKG_SOURCE)/src/linux/fs.cc $(INPKG_SOURCE)/src/linux/main.cc

.PHONY: all check check-install check-project check-optical install uninstall installer distribution
all: build/infoide

build/uninstall-script.h: script/uninstall.sh Makefile
	@mkdir -p "$(@D)"
	@{ printf '%s' 'R"SH('; cat "$<"; printf '%s\n' ')SH"'; } >"$@.tmp"
	@mv "$@.tmp" "$@"

build/infoide: src/main.cc $(wildcard src/*.h) build/uninstall-script.h $(PACKAGE_SOURCES) $(INPKG_SOURCE)/src/common/package.h $(INPKG_SOURCE)/include/inpkg/gzip.h Makefile
	@mkdir -p "$(@D)"
	@set -eu; temporary="$@.tmp.$$$$"; trap 'rm -f "$$temporary"' EXIT; \
		$(CXX) $(CXXFLAGS) -DINPKG_EMBEDDED -Ibuild -I"$(INPKG_SOURCE)/src/common" -I"$(INPKG_SOURCE)/include" src/main.cc $(PACKAGE_SOURCES) -o "$$temporary"; mv "$$temporary" "$@"

check: build/infoide
	OSINFO_SDK="$(abspath $(SDK))" sh "tests/cli.sh" "$(CURDIR)/build/infoide"

check-install: distribution
	OSINFO_EXPECT_CDROM="$(if $(ISO),1,0)" sh tests/distribution.sh

check-project: build/infoide
	OSINFO_SDK="$(abspath $(SDK))" OSINFO_DISK="$(abspath $(DISK))" sh tests/project.sh

check-optical: build/infoide
	@test -n "$(ISO)" || { echo 'Set WITH_CDROM=1 or ISO=/path/to/osinfo.iso' >&2; exit 2; }
	OSINFO_SDK="$(abspath $(SDK))" OSINFO_DISK="$(abspath $(DISK))" OSINFO_ISO="$(abspath $(ISO))" sh tests/optical.sh

installer: distribution

distribution: build/infoide
	@test -n "$(SDK)" -a -n "$(DISK)" || { echo 'Set SDK=/path/to/sdk DISK=/path/to/osinfo.img [ISO=/path/to/optional.iso]' >&2; exit 2; }
	@set -eu; stage=$$(mktemp -d build/.distribution.XXXXXX); trap 'rm -rf -- "$$stage"' EXIT; \
		build/infoide distribution --sdk "$(SDK)" --disk "$(DISK)" $(if $(ISO),--iso "$(ISO)") --output "$$stage/infoide-linux"; \
		mv -f "$$stage/infoide-linux.tar.gz" build/infoide-linux.tar.gz

install: distribution
	@set -eu; stage=$$(mktemp -d build/.install.XXXXXX); trap 'rm -rf -- "$$stage"' EXIT; \
		tar -xzf build/infoide-linux.tar.gz -C "$$stage"; \
		sh "$$stage/infoide-linux/install.sh" $(if $(DESTDIR),--destdir "$(DESTDIR)")

uninstall:
	sh script/uninstall.sh $(if $(DESTDIR),--destdir "$(DESTDIR)")
