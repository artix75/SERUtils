# Copyright (C) 2019  Giuseppe Fabio Nicotra <artix2 at gmail dot com>
SHELL=/bin/bash

default: all

.PHONY: clean

serutils:
	cd src && $(MAKE)
clean:
	rm -f src/*.o
	rm -f bin/*
	rm -f lib/*

install:
	cd src && $(MAKE) install
uninstall:
	cd src && $(MAKE) uninstall
all: serutils
	
        
