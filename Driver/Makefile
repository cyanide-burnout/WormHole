CONFIG_MODULE_SIG := n
obj-m             := uio_ivshmem.o

# kbuild runs this Makefile from the kernel tree, so dkms.conf has to be
# addressed through the directory this very Makefile was read from
HERE    := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
NAME    := $(shell sed -n 's/^PACKAGE_NAME="\(.*\)"/\1/p' $(HERE)/dkms.conf)
VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' $(HERE)/dkms.conf)
SRCDIR  := /usr/src/$(NAME)-$(VERSION)

# DKMS sets KERNELRELEASE when it rebuilds for a kernel other than the running
# one; without this the build would target uname -r instead.
KVER ?= $(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))
KDIR ?= /lib/modules/$(KVER)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	rm -f *.ko *.o .*.cmd *.mod* modules.order Module.symvers

# --- install ---------------------------------------------------------------

# plain module install: breaks on the next kernel upgrade, prefer `make dkms`
install: default
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a $(KVER)
	modprobe uio_ivshmem

# --- dkms ------------------------------------------------------------------

check-version:
	@test -n "$(VERSION)" || { echo "could not read PACKAGE_VERSION from dkms.conf" >&2; exit 1; }

# install or update; safe to re-run after editing the source
dkms: check-version
	install -D -m 0644 uio_ivshmem.c $(SRCDIR)/uio_ivshmem.c
	install -D -m 0644 Makefile      $(SRCDIR)/Makefile
	install -D -m 0644 dkms.conf     $(SRCDIR)/dkms.conf
	@dkms status -m $(NAME) -v $(VERSION) | grep -q . || dkms add -m $(NAME) -v $(VERSION)
	# build --force is required: install --force alone reinstalls the cached
	# object and silently skips compiling an edited source
	dkms build   -m $(NAME) -v $(VERSION) --force
	dkms install -m $(NAME) -v $(VERSION) --force
	@echo
	@echo "Then: make reload"

dkms-remove: check-version
	@dkms remove -m $(NAME) -v $(VERSION) --all 2>/dev/null || true
	rm -rf $(SRCDIR)

# drop every version, including ones left over from earlier installs
dkms-purge:
	@for v in $$(dkms status -m $(NAME) | sed -n 's/^$(NAME)[/,] *\([^,:]*\).*/\1/p' | sort -u); do \
		echo "removing $(NAME)/$$v"; \
		dkms remove -m $(NAME) -v $$v --all 2>/dev/null || true; \
	done
	rm -rf /usr/src/$(NAME)-*

reload:
	@if lsmod | grep -q '^uio_ivshmem '; then rmmod uio_ivshmem; fi
	modprobe uio_ivshmem
	@dmesg | grep -i ivshmem | tail -4

.PHONY: default clean install check-version dkms dkms-remove dkms-purge reload
