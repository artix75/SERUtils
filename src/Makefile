# Copyright (C) 2019  Giuseppe Fabio Nicotra <artix2 at gmail dot com>

SHELL=/bin/bash
CFLAGS=-std=gnu99 -pedantic -Wall -W -Wno-missing-field-initializers -Wno-unused-function -Wno-missing-braces
LDFLAGS=
LIBOPTS=
OBJS=ser.o log.o
PREFIX?=/usr/local
LIBDIR=$(PREFIX)/lib
BINDIR=$(PREFIX)/bin
INCLUDEDIR=$(PREFIX)/include
SHAREDIR=$(PREFIX)/share/psyc
PLATFORM := $(shell sh -c 'uname -s 2>/dev/null || echo not')
LIBNAME := libserutils

ifeq ($(PLATFORM), Linux)
        LIBNAME := $(LIBNAME).so
        LIBOPTS := -shared -fPIC -Wl,-soname,$(LIBNAME)
        CFLAGS+=-fPIC -Wno-unused-but-set-variable
endif

ifeq ($(PLATFORM), Darwin)
        LIBNAME := $(LIBNAME).dylib
        LIBOPTS := -dynamiclib -install_name $(LIBDIR)/$(LIBNAME)
endif

default: all
BIN_CFLAGS = $(CFLAGS)
BIN_LDFLAGS = $(LDFLAGS)
CLI_OBJS=$(OBJS) fits.o serutils.o

$(LIBNAME): $(OBJS)
	$(CC) $(LIBOPTS) -o ../lib/$@ $(OBJS) $(LDFLAGS)

serutils.o: 
	$(CC) $(BIN_CFLAGS) -c -o $@ serutils.c

serutils: $(CLI_OBJS)
	$(CC) -o ../bin/serutils $(CLI_OBJS) $(BIN_LDFLAGS)

all: serutils $(LIBNAME)

install: all
	@mkdir -p $(BINDIR)
	@mkdir -p $(LIBDIR)
	@mkdir -p $(INCLUDEDIR)/serutils
	cp ../bin/serutils $(BINDIR)/serutils
	cp ../lib/$(LIBNAME) $(LIBDIR)/$(LIBNAME)
	cp ser.h $(INCLUDEDIR)/serutils/
	cp log.h $(INCLUDEDIR)/serutils/
	cp version.h $(INCLUDEDIR)/serutils/
	cp serutils.h $(INCLUDEDIR)/serutils/
	@if [ "$(PLATFORM)" = "Linux" ]; then /sbin/ldconfig; fi

uninstall:
	rm -f $(BINDIR)/serutils
	rm -f $(LIBDIR)/$(LIBNAME)
	rm -rvf $(INCLUDEDIR)/serutils
