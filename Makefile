# SPDX-License-Identifier: AGPL-3.0-or-later
# VDR-Plugin rpi45 – Makefile

PLUGIN    = rpi45
VERSION   = $(shell grep 'static constexpr.*VERSION' common.h | sed -e 's/.*"\(.*\)".*/\1/')

### VDR-Umgebung
PKGCFG    = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),$(shell pkg-config --variable=$(1) vdr || pkg-config --variable=$(1) ../../../vdr.pc))
LIBDIR    = $(call PKGCFG,libdir)
LOCDIR    = $(call PKGCFG,locdir)
PLGCFG    = $(call PKGCFG,plgcfg)

TMPDIR    ?= /tmp

### Compiler
CXX       ?= g++
CXXFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
             -fstack-protector-strong -D_FORTIFY_SOURCE=2
override CXXFLAGS += -std=c++23 -fPIC \
    $(shell pkg-config --cflags vdr) \
    $(shell pkg-config --cflags libavcodec libavfilter libavutil libswresample) \
    $(shell pkg-config --cflags alsa)

LDFLAGS   += $(shell pkg-config --libs libavcodec libavfilter libavutil libswresample) \
             $(shell pkg-config --libs alsa) \
             -ldrm

### Quellen
OBJS = plugin.o device.o decoder.o display.o audio.o pes.o osd.o config.o

### Ziele
all: libvdr-$(PLUGIN).so

libvdr-$(PLUGIN).so: $(OBJS)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install: libvdr-$(PLUGIN).so
	install -D $< $(DESTDIR)$(LIBDIR)/$<.$(VERSION)

clean:
	@rm -f $(OBJS) libvdr-$(PLUGIN).so

.PHONY: all install clean
