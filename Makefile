# xpipe and xpick — X11 filters for a Unix pipeline
#
# Needs libX11 and libXft. On Debian/Ubuntu: apt install libx11-dev libxft-dev
# On Fedora: dnf install libX11-devel libXft-devel
# On macOS (XQuartz): xcode + brew or the XQuartz SDK under /opt/X11

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
PREFIX  ?= /usr/local

PKG_CFLAGS := $(shell pkg-config --cflags x11 xft 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs x11 xft 2>/dev/null)

ifeq ($(PKG_LIBS),)
  X11_CFLAGS ?= -I/usr/include -I/usr/include/freetype2 -I/opt/X11/include -I/opt/X11/include/freetype2
  X11_LIBS   ?= -lXft -lXrender -lX11 -lfontconfig -lfreetype
else
  X11_CFLAGS ?= $(PKG_CFLAGS)
  X11_LIBS   ?= $(PKG_LIBS)
endif

.PHONY: all clean install

all: xpipe xpick

xpipe: xpipe.c
	$(CC) $(CFLAGS) $(X11_CFLAGS) -o $@ xpipe.c $(X11_LIBS) -lm

xpick: xpick.c
	$(CC) $(CFLAGS) $(X11_CFLAGS) -o $@ xpick.c $(X11_LIBS) -lm

install: xpipe xpick
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 xpipe xpick $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f xpipe xpick
