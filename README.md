# WormHole

Linux inter-VM shared memory client library

* Fresh UIO driver compatible with Linux kernels 6.x
* Library could work
  * in host mode using direct connection to *QEMU ivshmem-server*
  * as well as in guest mode using UIO driver
* Allows to run multiple instances of application in both modes
* In addition here is integration with io_uring-based main loop (see repository **FastRing**)

## Driver

The UIO driver lives in `Driver/`. Building it needs the kernel headers for the
running kernel (`linux-headers-$(uname -r)` or equivalent).

Install through DKMS so the module is rebuilt automatically on kernel upgrades:

```
cd Driver
sudo make dkms
sudo make reload
```

To remove it:

```
sudo make dkms-remove   # this version
sudo make dkms-purge    # every installed version
```

A plain one-off build without DKMS is also available, but it does not survive
a kernel upgrade:

```
cd Driver
make
sudo make install
```

## Limits

* Only one IRQ vector is supported because of UIO limits
* Due to byte-order depended implementation of ivshmem in QEMU a guest system should use the same byte-order as a host

## Links

* https://www.qemu.org/docs/master/system/devices/ivshmem.html
* https://www.qemu.org/docs/master/specs/ivshmem-spec.html
* https://github.com/henning-schild-work/ivshmem-guest-code/tree/master
* https://github.com/qemu/qemu/tree/master/contrib/ivshmem-client
* https://github.com/projectacrn/acrn-hypervisor/tree/master/hypervisor/dm/vpci/ivshmem.c
* https://github.com/projectacrn/acrn-hypervisor/tree/master/misc/sample_application/rtvm
* https://github.com/torvalds/linux/blob/master/drivers/uio/uio_netx.c
