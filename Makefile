CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Werror
SDK ?= $(if $(OSINFO_SDK),$(OSINFO_SDK),resources/sdk)
DISK ?= $(if $(OSINFO_DISK),$(OSINFO_DISK),resources/osinfo.img)
WITH_CDROM ?= 0
ISO ?= $(if $(OSINFO_ISO),$(OSINFO_ISO),$(if $(filter 1,$(WITH_CDROM)),resources/osinfo.iso))
INPKG ?= $(if $(wildcard ../inpkg/build/linux/inpkg),../inpkg/build/linux/inpkg,inpkg)
INPKG_ENV = $(if $(findstring /,$(INPKG)),$(abspath $(INPKG)),$(INPKG))

.PHONY: all check check-dependencies check-install check-project check-optical check-inpkg install uninstall installer distribution
all: build/infoide

build/uninstall-script.h: script/uninstall.sh Makefile
	@mkdir -p "$(@D)"
	@{ printf '%s' 'R"SH('; cat "$<"; printf '%s\n' ')SH"'; } >"$@.tmp"
	@mv "$@.tmp" "$@"

build/infoide: src/main.cc $(wildcard src/*.h) build/uninstall-script.h Makefile
	@mkdir -p "$(@D)"
	@set -eu; temporary="$@.tmp.$$$$"; trap 'rm -f "$$temporary"' EXIT; \
		$(CXX) $(CXXFLAGS) -Ibuild src/main.cc -o "$$temporary"; mv "$$temporary" "$@"

check-inpkg:
	@command -v "$(INPKG)" >/dev/null 2>&1 || test -x "$(INPKG)" || { echo "missing host dependency: inpkg; install it or set INPKG=/path/to/inpkg" >&2; exit 1; }

check: build/infoide check-inpkg
	INPKG="$(INPKG_ENV)" OSINFO_SDK="$(abspath $(SDK))" sh "tests/cli.sh" "$(CURDIR)/build/infoide"

check-dependencies: build/infoide check-inpkg
	INPKG="$(INPKG_ENV)" OSINFO_SDK="$(abspath $(SDK))" OSINFO_DISK="$(abspath $(DISK))" $(if $(ISO),OSINFO_ISO="$(abspath $(ISO))") sh "tests/dependencies.sh"

check-install: distribution check-inpkg
	INPKG="$(INPKG_ENV)" OSINFO_EXPECT_CDROM="$(if $(ISO),1,0)" sh tests/distribution.sh

check-project: build/infoide check-inpkg
	INPKG="$(INPKG_ENV)" OSINFO_SDK="$(abspath $(SDK))" OSINFO_DISK="$(abspath $(DISK))" sh tests/project.sh

check-optical: build/infoide check-inpkg
	@test -n "$(ISO)" || { echo 'Set WITH_CDROM=1 or ISO=/path/to/osinfo.iso' >&2; exit 2; }
	INPKG="$(INPKG_ENV)" OSINFO_SDK="$(abspath $(SDK))" OSINFO_DISK="$(abspath $(DISK))" OSINFO_ISO="$(abspath $(ISO))" sh tests/optical.sh

installer: distribution

distribution: build/infoide check-inpkg
	@test -n "$(SDK)" -a -n "$(DISK)" || { echo 'Set SDK=/path/to/sdk DISK=/path/to/osinfo.img [ISO=/path/to/optional.iso]' >&2; exit 2; }
	@set -eu; stage=$$(mktemp -d build/.distribution.XXXXXX); trap 'rm -rf -- "$$stage"' EXIT; \
		INPKG="$(INPKG_ENV)" build/infoide distribution --sdk "$(SDK)" --disk "$(DISK)" $(if $(ISO),--iso "$(ISO)") --output "$$stage/infoide-linux"; \
		mv -f "$$stage/infoide-linux.tar.gz" build/infoide-linux.tar.gz

install: distribution
	@set -eu; stage=$$(mktemp -d build/.install.XXXXXX); trap 'rm -rf -- "$$stage"' EXIT; \
		tar -xzf build/infoide-linux.tar.gz -C "$$stage"; \
		sh "$$stage/infoide-linux/install.sh" $(if $(DESTDIR),--destdir "$(DESTDIR)")

uninstall:
	sh script/uninstall.sh $(if $(DESTDIR),--destdir "$(DESTDIR)")
