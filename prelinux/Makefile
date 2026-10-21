# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# PreLinux base SDK — build the `prelinux` CLI (musl-static) and
# libprelinux.so (dynamic, for SDK consumers running inside instances).
#
# The CLI is statically linked against musl, NOT glibc: static-glibc
# binaries die at startup on HarmonyOS PC (its kernel SIGSYS-kills the
# legal syscalls glibc registers early, e.g. rseq); musl is HarmonyOS's
# native userland libc, so a musl-static binary runs on both HongMeng
# and Linux kernels unchanged.
#
# Usage: make [VERSION=x.y.z] [MUSLCC=musl-gcc] [CC=cc] [DESTDIR=]
# VERSION becomes `prelinux --version`; release builds pass the tag.
MUSLCC ?= musl-gcc
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
STATIC ?= -static
VERSION ?= 0.0.0-dev
VERSION_H = obj-cli/prelinux-version.h

CLI_SRCS = src/main.c src/translate.c src/relocate.c src/import.c \
           src/overlay.c src/elf.c src/selfsign.c src/cliutil.c
MUSL_OBJS = $(patsubst src/%.c,obj-cli/%.o,$(CLI_SRCS))

all: prelinux libprelinux.so

prelinux: $(MUSL_OBJS)
	$(MUSLCC) $(STATIC) $(CFLAGS) -o $@ $(MUSL_OBJS)

# CLI objects are compiled with the musl toolchain (headers differ).
obj-cli/%.o: src/%.c src/prelinux.h src/prelinux-translate.h
	@mkdir -p obj-cli
	$(MUSLCC) $(CFLAGS) -c -o $@ $<

# The version string is generated: rewriting it only when it changes
# keeps repeated builds from rebuilding everything.
$(VERSION_H): FORCE
	@mkdir -p obj-cli
	@printf '#define PRELINUX_VERSION "%s"\n' \
	  '$(if $(strip $(VERSION)),$(VERSION),0.0.0-dev)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

obj-cli/main.o: src/main.c src/prelinux.h src/prelinux-translate.h $(VERSION_H)
	@mkdir -p obj-cli
	$(MUSLCC) $(CFLAGS) -Iobj-cli -c -o $@ src/main.c

FORCE:
.PHONY: FORCE

# ohos-selfsign vendored as a library (its main() is compiled out).
obj-cli/selfsign.o: src/selfsign.c src/selfsign.h
	@mkdir -p obj-cli
	$(MUSLCC) $(CFLAGS) -DSELFSIGN_AS_LIBRARY -c -o $@ $<

# libprelinux.so exports prelinux_root()/prelinux_translate() (design
# 5.9) for SDK tools; it is compiled from the same translation core as
# the glibc patches (tests/test-sync.sh keeps them in sync).
libprelinux.so: src/translate.c src/prelinux-translate.h src/prelinux.h
	$(CC) -shared -fPIC -fvisibility=hidden \
	  $(CFLAGS) -o $@ src/translate.c \
	  -Wl,-soname,libprelinux.so,-z,defs

clean:
	rm -f prelinux libprelinux.so
	rm -rf obj-cli

.PHONY: all clean
