# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stephen Olesen

PACKAGE := i2ckiss-ng
VERSION := $(shell sed -n '1p' VERSION)
DIST_SOURCES := $(shell sed '/^$$/d' packaging/source-files.txt)

CC ?= cc
ARM_CC ?= arm-linux-gnueabihf-gcc
STRIP ?= strip
ARM_STRIP ?= arm-linux-gnueabihf-strip
CFLAGS ?= -O2 -g
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
VERSION_CPPFLAG := -DPROGRAM_VERSION=\"$(VERSION)\"

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
DATADIR ?= $(PREFIX)/share
SYSTEMD_UNIT_DIR ?= $(PREFIX)/lib/systemd/system

SOURCE_DATE_EPOCH ?= 1788134400
export SOURCE_DATE_EPOCH

.PHONY: all clean test check install arm dist dist-dir package-deb \
	package-deb-armhf packages checksums

all: build/i2ckiss build/i2ckiss@.service

build:
	mkdir -p $@

dist-dir:
	mkdir -p dist

build/i2ckiss: src/i2ckiss.c VERSION | build
	$(CC) $(CPPFLAGS) $(VERSION_CPPFLAG) $(CFLAGS) $(WARNINGS) -std=c11 -o $@ $<

build/test_i2ckiss: tests/test_i2ckiss.c src/i2ckiss.c VERSION | build
	$(CC) $(CPPFLAGS) $(VERSION_CPPFLAG) $(CFLAGS) $(WARNINGS) \
		-Wno-unused-function -std=c11 -o $@ tests/test_i2ckiss.c

build/i2ckiss.armhf: src/i2ckiss.c VERSION | build
	$(ARM_CC) $(CPPFLAGS) $(VERSION_CPPFLAG) $(CFLAGS) $(WARNINGS) \
		-std=c11 -o $@ $<

build/i2ckiss@.service: systemd/i2ckiss@.service.in | build
	sed 's|@SBINDIR@|$(SBINDIR)|g' $< > $@

test check: build/i2ckiss build/test_i2ckiss
	./build/test_i2ckiss
	sh tests/test_lifecycle.sh ./build/i2ckiss

arm: build/i2ckiss.armhf

install: all
	install -Dm755 build/i2ckiss $(DESTDIR)$(SBINDIR)/i2ckiss
	install -Dm644 build/i2ckiss@.service \
		$(DESTDIR)$(SYSTEMD_UNIT_DIR)/i2ckiss@.service
	install -Dm644 systemd/i2ckiss.example.conf \
		$(DESTDIR)/etc/i2ckiss/example.conf
	install -Dm644 docs/i2ckiss.8 \
		$(DESTDIR)$(DATADIR)/man/man8/i2ckiss.8
	install -Dm644 LICENSE README.md CHANGELOG.md \
		-t $(DESTDIR)$(DATADIR)/doc/$(PACKAGE)
	install -Dm644 docs/device-aliases.md docs/migration.md \
		docs/multiple-tncs.md \
		-t $(DESTDIR)$(DATADIR)/doc/$(PACKAGE)/docs
	install -Dm644 systemd/i2ckiss-tmpfiles.example.conf \
		$(DESTDIR)$(DATADIR)/doc/$(PACKAGE)/examples/i2ckiss-tmpfiles.conf

dist/$(PACKAGE)-$(VERSION).tar.gz: packaging/source-files.txt $(DIST_SOURCES) | dist-dir
	./packaging/build-source.sh $@

dist: dist/$(PACKAGE)-$(VERSION).tar.gz

package-deb: all | dist-dir
	VERSION=$(VERSION) BINARY=build/i2ckiss STRIP=$(STRIP) \
		DEB_ARCH=$$(dpkg --print-architecture) \
		./packaging/build-deb.sh

package-deb-armhf: build/i2ckiss.armhf | dist-dir
	VERSION=$(VERSION) BINARY=build/i2ckiss.armhf STRIP=$(ARM_STRIP) \
		DEB_ARCH=armhf \
		./packaging/build-deb.sh

checksums: | dist-dir
	umask 022; cd dist && find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\n' \
		| LC_ALL=C sort | xargs sha256sum > SHA256SUMS
	chmod 0644 dist/SHA256SUMS

packages: dist package-deb package-deb-armhf
	$(MAKE) checksums

clean:
	rm -rf build dist
