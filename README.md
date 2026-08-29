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

## Library

### WormHole

```c
struct WormHole* CreateWormHole(const char* path, HandleWormHoleBellFunction function, void* closure);
void ReleaseWormHole(struct WormHole* hole);

int KickWormHole(struct WormHole* hole, uint32_t identifier);

int HandleWormHoleHostServerEvent(struct WormHole* hole);
void HandleWormHoleInterruptEvent(struct WormHole* hole, int64_t value);
```

`CreateWormHole` picks the mode by what `path` turns out to be: a character device
is the UIO driver and gives a guest client, a socket is *ivshmem-server* and gives a
host one. Everything below that difference is the same for both.

What the caller gets to use is `memory` and `length` — the shared region itself —
and `identifier`, the ID of this peer among the others. `KickWormHole` rings the
doorbell of a peer by its ID, and the `function` given at creation is called when
somebody rings ours.

Waiting is left to the caller: `handle` is the event source to poll and `size` is
how much to read from it. Having read a value, hand it to
`HandleWormHoleInterruptEvent`; hand it `HOLE_CLIENT_VALUE_READ` instead to have the
source read for you. In host mode the connection to the server is a second source,
`host.socket`, and everything that arrives on it is for
`HandleWormHoleHostServerEvent` — that is how the peers coming and going become
known.

A peer may ring itself, and the mode makes no difference to that: on a host several
clients of one server see each other the same way as the machines do.

### WormLoop

```c
struct WormLoop* CreateWormLoop(struct FastRing* ring, struct WormHole* hole);
void ReleaseWormLoop(struct WormLoop* loop);
```

Ties a hole to an io_uring loop of **FastRing**, and after that neither source has to
be polled by hand: the doorbell arrives as the bell of the hole, and in host mode the
socket of the server is watched as well. The kick is replaced by an io_uring one, so
ringing a neighbour costs no syscall of its own.

The rule of a loop applies: the bell is called from inside the loop and has nothing
to wait for there.

## Arena

On top of the library there is **WormArena** — a store of named objects, pools of
blocks and lock-free queues laid out inside the shared memory, meant to be used by
several processes on several machines at once. See [Arena.md](Arena.md).

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
