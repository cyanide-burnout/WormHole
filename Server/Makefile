# Front door for install.sh; everything it takes is an option of that script,
# see ./install.sh --help
#
#   make install                     build and install, with one "default" instance
#   make install FLAGS="--size 64M --hook"
#   make build                       build the binary and stop
#   make uninstall

PREFIX  ?= /usr/local
FLAGS   ?=

install:
	./install.sh --prefix $(PREFIX) $(FLAGS)

build:
	./install.sh --build-only $(FLAGS)

uninstall:
	./uninstall.sh --prefix $(PREFIX)

purge:
	./uninstall.sh --prefix $(PREFIX) --purge

list:
	$(PREFIX)/sbin/ivshmem-server-ctl list

.PHONY: install build uninstall purge list
