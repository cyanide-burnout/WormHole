# Guest Mode

This document describes what a virtual machine needs for guest mode: the *ivshmem*
device attached to it, by QEMU directly or through libvirt, the UIO driver inside it,
and how to check that the two have met. The server the device connects to is described
in [SERVER.md](SERVER.md), the library itself in the [README](../README.md).

- [Attaching the Device](#attaching-the-device)
  - [QEMU](#qemu)
  - [libvirt](#libvirt)
- [Driver](#driver)
  - [Building and Installing](#building-and-installing)
  - [What the Driver Does](#what-the-driver-does)
  - [Makefile](#makefile)
- [Checking](#checking)

## Attaching the Device

The device is *ivshmem-doorbell*, connected to the socket of an instance of
*ivshmem-server*, with one vector. QEMU connects to the socket while the device is being
realized, so the server has to be up before the machine starts (see
[What libvirt will not do for you](SERVER.md#what-libvirt-will-not-do-for-you)).

### QEMU

Started by hand, as `Test/Hole/test.sh` does, the device is a pair of options:

```
-device ivshmem-doorbell,vectors=1,chardev=ivshmem0
-chardev socket,path=/run/ivshmem-server/default.sock,id=ivshmem0
```

### libvirt

A domain is given the ordinary thing:

```xml
<shmem name='ivshmem-default'>
  <model type='ivshmem-doorbell'/>
  <size unit='M'>16</size>
  <server path='/run/ivshmem-server/default.sock'/>
  <msi vectors='1'/>
</shmem>
```

One vector, because that is all the UIO driver can make use of. On a host with
AppArmor, `/run/...` is outside what the generated profile grants a domain; if
the domain is denied the socket, either put it where the profile reaches or add
the path to `/etc/apparmor.d/local/abstractions/libvirt-qemu`.

## Driver

### Building and Installing

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


### What the Driver Does

`uio_ivshmem` binds to the PCI device of *ivshmem*, `1af4:1110`, and registers it as a
UIO device named `uio_ivshmem`, which is the name `CreateWormHole` checks in sysfs
before taking a character device for one. The device has two maps:

| Map | Name | Content |
|-----|------|---------|
| 0 | `control` | The registers of BAR0, rounded up to a page; the library reads the ID from them and rings the doorbell through them |
| 1 | `memory` | The shared region of BAR2 |

The driver asks for exactly one interrupt vector: MSI-X where the device has it, INTx
otherwise, registered as a shared interrupt. That is why a domain is given
`<msi vectors='1'/>`, and why only one vector is supported (see
[Limits](../README.md#limits)). Without MSI-X the driver writes `1` into the interrupt
mask register once the device is registered, and its handler reads the status register
to tell an interrupt of this device from one of another device on the same line,
answering `IRQ_NONE` when the status is zero. Every interrupt it accepts is delivered
by UIO to the processes reading the device.

### Makefile

| Target | What it does |
|--------|--------------|
| `default` | Builds the module against `KDIR` |
| `install` | Builds, installs with `modules_install`, runs `depmod -a` and loads the module |
| `dkms` | Copies the sources to `/usr/src/uio-ivshmem-1.0`, adds the package to DKMS if it is not there, then builds and installs it with `--force` |
| `dkms-remove` | Removes this version from DKMS for every kernel and deletes its sources |
| `dkms-purge` | Does the same for every version DKMS knows of and deletes all of `/usr/src/uio-ivshmem-*` |
| `reload` | Unloads the module if it is loaded, loads it again and shows the last lines about *ivshmem* from `dmesg` |
| `clean` | Removes the build products |

`dkms` is safe to run again after editing the source. `dkms build --force` is what makes
that so: `dkms install --force` alone reinstalls the cached object and skips compiling
the edited source.

The package name and version are read from `dkms.conf`: `uio-ivshmem` `1.0`, installed
under `/kernel/drivers/uio` and rebuilt by DKMS for every new kernel (`AUTOINSTALL`).
The kernel built for is `KVER`, which is `KERNELRELEASE` when DKMS sets it and
`uname -r` otherwise, and its tree is `KDIR`, `/lib/modules/$(KVER)/build`; either can be
given on the command line. The Makefile sets `CONFIG_MODULE_SIG := n` for the build.

## Checking

Once the module is loaded, the device appears as `/dev/uio0`, and sysfs says what
`CreateWormHole` is going to check: the name of the UIO device and the size of map 1,
which is the size of the region, in hexadecimal.

```
cat /sys/class/uio/uio0/name
cat /sys/class/uio/uio0/maps/map1/size
```

The name has to be `uio_ivshmem`. A process that opens the device needs to be able to
read and write it, since `CreateWormHole` opens it for both and maps the registers as
well as the region.
