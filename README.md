# WormHole

WormHole is a C library for Linux that lets processes on a host and inside its virtual
machines share a region of memory and ring each other's doorbells, over *ivshmem* of
QEMU. Both sides of that come with it: a fresh UIO driver for kernels 6.x on the side
of a guest, and on the side of the host *ivshmem-server*, which QEMU builds out of its
sources and installs nowhere, put under systemd with an instance per shared region.

- [The Problem](#the-problem)
- [Architecture](#architecture)
- [Getting Started](#getting-started)
- [Library](#library)
- [Execution and Lifecycle Contract](#execution-and-lifecycle-contract)
- [Arena](#arena)
- [Deployment](#deployment)
- [Testing](#testing)
- [Limits](#limits)
- [Documentation](#documentation)
- [Links](#links)

## The Problem

Processes on one machine, some on the host and some in its virtual machines, can work
on the same data in memory they share. QEMU offers that as *ivshmem*: a region backed by
a shared object on the host and presented to a guest as a PCI device, and, with
*ivshmem-doorbell*, a way for every participant to interrupt any other. What it leaves
out is everything around that:

- The server that hands out the region and the doorbells is built by QEMU and installed
  nowhere, so no distribution carries it.
- A guest needs a driver to reach the device from user space.
- A program on the host speaks the protocol of the server, and a program in a guest
  speaks to the registers of the device: two interfaces for the same thing.
- The memory is all there is. Futexes, and with them every blocking primitive of the
  system, do not cross the boundary of a virtual machine.

WormHole covers each of them: `Server/` builds the server and puts it under systemd,
`Driver/` is the UIO driver, the library is one client for both sides, and WormArena
builds shared objects out of nothing but atomic operations and the doorbell.

## Architecture

Every participant is a *peer* of one instance of *ivshmem-server*, which hands each of
them the region and the eventfds of all the others. A process on the host connects to
the socket of the server itself; that is *host mode*. A virtual machine is connected by
QEMU through an *ivshmem-doorbell* device, and a process inside it opens that device
through the UIO driver; that is *guest mode*.

```
  process on the host                       process in a guest
  WormHole, host mode                       WormHole, guest mode
          |                                         | /dev/uio0
          |                                   uio_ivshmem (UIO driver)
          |                                         | PCI
          |                                   ivshmem-doorbell (QEMU)
          | socket                                  | socket
          +------------->  ivshmem-server  <--------+
                    the region, an eventfd per peer
```

Every peer has an ID, and a doorbell is rung by ID. Several processes may run in either
mode at once: on the host each of them is a peer of its own, while in a guest they share
the device and its ID, so a machine is a single peer however many processes it runs.

Three layers sit on top of that:

**WormHole** is the client of a region. `CreateWormHole` takes the socket or the UIO
device and gives the memory, the ID of this peer, a way to ring another peer and a
callback for when somebody rings this one. The waiting is left to the caller.

**WormLoop** ties a hole to an io_uring loop of **FastRing**, so that the doorbell
and, in host mode, the connection to the server are watched by the loop instead of
being polled by hand.

**WormArena** lays out named objects, pools of blocks and lock-free queues inside the
region, for several processes on several machines at once, with leases and sweeps in
place of anything that would need a single kernel.

## Getting Started

There is no build system for the library: its sources are compiled into the application,
as the tests do. `WormHole.c` needs nothing but libc; `WormLoop.c` needs `FastRing.c` of
**FastRing** and liburing; `WormArena.c` needs `HashMap` and `CRC32C` of the Toolkit of
TetraPack. The guest needs the driver and the host a running server (see
[Deployment](#deployment)).

Two peers, each given the path of its side and the ID of the other, write a line into
the region once a second and ring each other; a bell prints what the other has written:

```c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "FastRing.h"
#include "WormHole.h"
#include "WormLoop.h"

#define SLOT_SIZE  64

static void HandleBell(struct WormHole* hole)
{
  uint32_t* peer;

  // A bell does not say who rang, what has changed is found in the memory
  peer = (uint32_t*)hole->closure;
  printf("%s\n", (char*)hole->memory + *peer * SLOT_SIZE);
}

static void HandleTimeout(struct FastRingDescriptor* descriptor)
{
  struct WormHole* hole;
  uint32_t* peer;

  hole = (struct WormHole*)descriptor->closure;
  peer = (uint32_t*)hole->closure;

  snprintf((char*)hole->memory + hole->identifier * SLOT_SIZE, SLOT_SIZE, "Hello from %u", (uint32_t)hole->identifier);
  KickWormHole(hole, *peer);
}

static void HandleSignal(int signal)
{
  // Nothing to do here, the signal itself makes WaitForFastRing() return an error
}

int main(int count, char** arguments)
{
  struct FastRingDescriptor* timer;
  struct FastRing* ring;
  struct WormHole* hole;
  struct WormLoop* loop;
  uint32_t peer;

  // The socket of ivshmem-server on the host, /dev/uio0 in a guest
  peer = strtoul(arguments[2], NULL, 10);
  hole = CreateWormHole(arguments[1], HandleBell, &peer);

  printf("This is peer %u\n", (uint32_t)hole->identifier);

  ring  = CreateFastRing(0);
  loop  = CreateWormLoop(ring, hole);
  timer = SetFastRingTimeout(ring, NULL, 1000, TIMEOUT_FLAG_REPEAT, HandleTimeout, hole);

  signal(SIGINT, HandleSignal);

  while (WaitForFastRing(ring, 200, NULL) >= 0);

  // Reverse order: the loop goes while the ring is still there, the hole goes last
  SetFastRingTimeout(ring, timer, -1, 0, NULL, NULL);
  ReleaseWormLoop(loop);
  ReleaseFastRing(ring);
  ReleaseWormHole(hole);

  return EXIT_SUCCESS;
}
```

In host mode the first kicks may return `-EBADF`: the peers become known only once the
loop has read the socket of the server. `Test/Hole` is the full version of this.

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
socket of the server is watched as well. The kick stays what it is: in both modes the
doorbell has been rung by the time `KickWormHole` returns.

The rule of a loop applies: the bell is called from inside the loop and has nothing
to wait for there.

Every function, field and constant is listed in [API.md](Documentation/API.md).

## Execution and Lifecycle Contract

`WormHole` and `WormLoop` take no locks and start no threads. The rules that follow from
that, and from where each mode gets its events, are in
[LIFECYCLE.md](Documentation/LIFECYCLE.md); in short:

- **A hole belongs to its creator** and is used from one thread, except for
  `KickWormHole`, which any thread may call. A `WormLoop` and a `WormArena` refer to it
  and never release it.
- **A host hole is not ready when `CreateWormHole` returns.** Its event source and the
  eventfds of the peers arrive on `host.socket`, and until that has been read no bell is
  heard and no peer can be kicked.
- **A bell says that somebody has rung and no more**: neither who nor how many times.
- **A kick reaches a peer the server has announced** in host mode and returns `-EBADF`
  otherwise; in guest mode it always returns `0`. In both modes the doorbell has been
  rung by the time it returns.
- **Losses are reported, not repaired.** A peer that leaves is dropped from the table;
  the loss of the server or of the event source is reported through `loss`, by the loop
  or by the caller, and nothing reconnects.
- **Release goes in reverse**: the loop before the ring, the arena before the hole.

## Arena

On top of the library there is **WormArena** — a store of named objects, pools of
blocks and lock-free queues laid out inside the shared memory, meant to be used by
several processes on several machines at once. See [ARENA.md](Documentation/ARENA.md).

## Deployment

### Guest

A virtual machine needs an *ivshmem-doorbell* device connected to an instance of the
server, and inside it the UIO driver in `Driver/`, built against the headers of the
running kernel and installed through DKMS so that it is rebuilt on kernel upgrades:

```
cd Driver
sudo make dkms
sudo make reload
```

Attaching the device by QEMU or libvirt, removing the driver or building it without
DKMS, what it makes of the device and how to check the result are described in
[Documentation/GUEST.md](Documentation/GUEST.md).

### Server

Host mode needs *ivshmem-server*, which no distribution carries. `Server/` builds it out
of the QEMU sources and puts it under systemd with an instance per region:

```
cd Server
sudo ./install.sh
```

The build, the instances and their configuration, and what has to be done for libvirt
are described in [Documentation/SERVER.md](Documentation/SERVER.md).

## Testing

`Test/Arena` needs neither the driver nor a server: an anonymous shared mapping
stands in for ivshmem and `fork` for the peers. It covers the layout and the leases,
the pools, the queues and the descriptors, and four ways of a peer dying in the
middle of something — 72 checks in all.

```
cd Test/Arena
make && ./arena
```

`Test/Hole` is the one that wants the real thing. `test.sh 1` runs host clients
against *ivshmem-server*, `test.sh 2` builds a guest image and boots it with
*ivshmem-doorbell* so that the guest talks to the host through the driver.
*ivshmem-server* is not packaged by Debian; `Server/` builds it out of the
`contrib/` directory of QEMU and puts it under systemd, as described in
[Documentation/SERVER.md](Documentation/SERVER.md).

`Test/Smoke` puts the arena over that same real region. Every peer is given the path
of the socket or of the UIO device, a name of its own, a number of seconds and how
many peers take part in the run, itself included:

```
cd Test/Smoke
make && ./smoke /tmp/ivshmem_socket first 30 6
```

Whoever gets there first lays the arena out and the rest attach to it by name. Each
keeps a lease, takes the write lock of a shared region once a second, and hears from
the others two ways at once. It has a queue of its own, named after it, that everybody
else puts a message into every second; and there is one queue shared by all of them
that each posts to and takes from as well.

The verdict stands on the first of the two. Nobody competes for an inbox, so hearing
from every other peer is a matter of the arena working rather than of winning a race,
and a peer left alone is not mistaken for one that has reached somebody. The shared
queue is reported and not judged, because competing for one queue is where the
behaviour worth looking at shows up — and it is not fair. Consumers wake at very
different speeds: a peer of the host, woken by a write to an eventfd inside its own
kernel, was measured taking two thirds of everything, while a peer of a guest, which
waits on a doorbell through a virtual machine exit, took none of it at all. The
report also counts how often taking failed on a slot a producer had claimed and not
yet filled in, which is the other way a queue can look empty when it is not.

### What has been run

On 2026-08-30, Debian 13 and kernel 6.12.107 on both sides of the boundary:

* Host mode, several clients on one *ivshmem-server*: doorbells travel both ways,
  and when the server is killed the loss is reported once and the client falls
  silent instead of spinning.
* Guest mode through the driver: the module loads, `/dev/uio0` appears with a
  region of the expected size, and the bells cross the boundary of the virtual
  machine in both directions.
* Twelve clients at once — four on the host and four in each of two virtual
  machines — and every one of them sees all twelve sources. A machine is a single
  peer however many processes it runs: the four in a guest share one ID, while the
  four on the host have one each.
* All four processes holding one `/dev/uio0` are woken by an interrupt, not merely
  the first of them.
* No io-wq worker is left parked in a blocking read. Measured against the previous
  revision, which had one per hole: the UIO device supports no NOWAIT reads, so an
  asynchronous read of it is served by a kernel worker, whereas watching the source
  and reading it once ready is not.
* The arena over that region, peers of `Test/Smoke` on the host and in both virtual
  machines at once, some of them joining half a minute after the arena had been laid
  out and had been busy since. One laid it out and the rest attached to it by name,
  every one of them held a lease and took the write lock of the region without missing
  it once, and every one of them heard from every other through its own queue. Nothing
  was ever lost in either direction: 1070 messages sent between the inboxes and 1070
  taken out of them, 345 posted to the shared queue and 345 taken from it, and both
  left empty at the end.
* Taking from a shared queue is not fair, and it was never once held up by a slot
  a producer had claimed and not filled in. Of the 345 messages a peer of the host
  took 220 and a peer of a guest took none, which is the difference between waking
  inside a kernel and waking through a virtual machine exit.

What has not been run: the ways a peer can die in the middle of something — the
sweeps that give back its blocks, its locks and the queue slots it took and never
filled — are covered only by `Test/Arena`, where the memory is ordinary and cacheable
and the peers are forks of one process.

## Limits

* Only one IRQ vector is supported because of UIO limits
* Due to byte-order depended implementation of ivshmem in QEMU a guest system should use the same byte-order as a host

## Documentation

- [API.md](Documentation/API.md): functions, fields, constants and callbacks of
  `WormHole` and `WormLoop`.
- [LIFECYCLE.md](Documentation/LIFECYCLE.md): ownership, coming up in each mode, peers,
  bells, kicks, losses and release.
- [ARENA.md](Documentation/ARENA.md): the model, the interface and the rules of
  `WormArena`.
- [GUEST.md](Documentation/GUEST.md): attaching the device by QEMU or libvirt, building and
  installing the UIO driver, what it does, and checking the result.
- [SERVER.md](Documentation/SERVER.md): building *ivshmem-server*, its instances under
  systemd, and libvirt.

## Links

* https://www.qemu.org/docs/master/system/devices/ivshmem.html
* https://www.qemu.org/docs/master/specs/ivshmem-spec.html
* https://github.com/henning-schild-work/ivshmem-guest-code/tree/master
* https://github.com/qemu/qemu/tree/master/contrib/ivshmem-client
* https://github.com/projectacrn/acrn-hypervisor/tree/master/hypervisor/dm/vpci/ivshmem.c
* https://github.com/projectacrn/acrn-hypervisor/tree/master/misc/sample_application/rtvm
* https://github.com/torvalds/linux/blob/master/drivers/uio/uio_netx.c
