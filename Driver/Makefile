CONFIG_MODULE_SIG := n
obj-m             := uio_ivshmem.o
KDIR              := /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install:
	cp uio_ivshmem.ko /lib/modules/$(shell uname -r)/kernel/drivers/uio/
	depmod
	modprobe uio_ivshmem

clean:
	rm -f *.ko *.o .*.cmd *.mod* modules.order Module.symvers
