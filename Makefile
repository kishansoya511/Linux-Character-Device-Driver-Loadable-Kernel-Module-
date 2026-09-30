# Makefile for mychardev.ko
#
# This does NOT compile mychardev.c directly with gcc. Instead it invokes
# the running kernel's own build system (Kbuild), which already knows the
# correct compiler flags, headers, and struct layouts for THIS kernel.

obj-m += mychardev.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
