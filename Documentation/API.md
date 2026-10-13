# API

This document lists the interface of `WormHole` (`Library/WormHole.h`) and `WormLoop`
(`Library/WormLoop.h`): the functions, the fields of a hole that the caller uses, the
constants and the events. When each of them may be called and what happens around
them is in [LIFECYCLE.md](LIFECYCLE.md); the interface of `WormArena` is in
[ARENA.md](ARENA.md#interface), the overall model in the [README](../README.md).

- [WormHole](#wormhole)
  - [Constants](#constants)
  - [Events](#events)
  - [Structure](#structure)
  - [Functions](#functions)
- [WormLoop](#wormloop)

## WormHole

```c
struct WormHole* CreateWormHole(const char* path, HandleWormHoleEventFunction function, void* closure);
void ReleaseWormHole(struct WormHole* hole);

int KickWormHole(struct WormHole* hole, uint32_t identifier);

int HandleWormHoleServerEvent(struct WormHole* hole);
void HandleWormHoleBellEvent(struct WormHole* hole, int64_t value);
void HandleWormHoleLossEvent(struct WormHole* hole, int value);

#define IsWormHoleReady(hole)
```

### Constants

| Name | Value | Meaning |
|------|-------|---------|
| `HOLE_CLIENT_TYPE_GUEST` | `1` | `type` of a hole opened through the UIO driver |
| `HOLE_CLIENT_TYPE_HOST` | `2` | `type` of a hole connected to *ivshmem-server* |
| `HOLE_CLIENT_VALUE_READ` | `INT64_MIN` | Passed to `HandleWormHoleBellEvent` to have the event source read by it |
| `HOLE_EVENT_READY` | `0` | Host mode only: the server has granted the event source of this peer |
| `HOLE_EVENT_LOST` | `1` | The connection to the server or the event source is gone |
| `HOLE_EVENT_BELL` | `2` | Somebody has rung this peer |
| `HOLE_LOST_SERVER` | `0` | Value of `HOLE_EVENT_LOST`: the connection to the server is gone |
| `HOLE_LOST_SOURCE` | `1` | Value of `HOLE_EVENT_LOST`: the event source is gone; in guest mode, the device has been unplugged |

### Events

```c
typedef void (*HandleWormHoleEventFunction)(struct WormHole* hole, int event, int value);
```

The function is given to `CreateWormHole` and is called with one of the events:

| Event | Called by | `value` |
|-------|-----------|---------|
| `HOLE_EVENT_READY` | `HandleWormHoleServerEvent`, once, when the server has granted the event source of this peer | The ID of this peer |
| `HOLE_EVENT_LOST` | `HandleWormHoleLossEvent`, which `HandleWormHoleServerEvent` calls when the server is gone | `HOLE_LOST_SERVER` or `HOLE_LOST_SOURCE` |
| `HOLE_EVENT_BELL` | `HandleWormHoleBellEvent` | `0` |

By `HOLE_EVENT_READY` the eventfds of the peers that were there before have arrived, so
they can be kicked. A guest hole is ready as soon as it is created and is not told so.

The function may be `NULL`, and is then not called. What it needs is reached through
`closure`.

`IsWormHoleReady` tells at any moment whether the hole has its event source: always in
guest mode, after `HOLE_EVENT_READY` in host mode.

`ReleaseWormHoleFunction` and `KickWormHoleFunction` are the types of the `release` and
`kick` fields, which the mode fills in; the caller goes through `ReleaseWormHole` and
`KickWormHole` instead.

### Structure

The fields the caller reads or sets:

| Field | Type | Meaning |
|-------|------|---------|
| `type` | `int` | `HOLE_CLIENT_TYPE_GUEST` or `HOLE_CLIENT_TYPE_HOST` |
| `closure` | `void*` | The `closure` given to `CreateWormHole`, for the events |
| `identifier` | `int64_t` | ID of this peer |
| `memory` | `uint8_t*` | The shared region |
| `length` | `size_t` | Size of the shared region |
| `handle` | `int` | Event source that delivers the bell; `-1` in host mode until the server has granted it |
| `size` | `size_t` | How much to read from `handle`: 4 bytes in guest mode, 8 in host mode |
| `host.socket` | `int` | Connection to *ivshmem-server*, host mode only |

The rest belongs to the library: `kick`, `release` and `event`, the function given to
`CreateWormHole`; `host.memory`, the descriptor of the shared object; `host.handles`, the
eventfds of all peers by ID, `-1` where there is none, each packed with the count of
kicks writing to it and a bit for a peer that has gone (see [Kicks](LIFECYCLE.md#kicks));
and `guest.registers`, the mapping of BAR0 of the device. `extension` is not used by the
library.

### Functions

**`CreateWormHole`** picks the mode by what `path` is. A character device is taken for
the UIO device and gives a guest hole: the device is opened, its name in sysfs has to be
`uio_ivshmem`, map 1 is mapped as the region and map 0 as the registers, and the ID is
read from the `IVPosition` register. A socket is taken for *ivshmem-server* and gives a
host hole: the client connects, expects protocol version `0`, receives its ID and the
descriptor of the shared object, and maps the whole object. `function` is called with
the events and `closure` is stored in the hole. Returns `NULL` when `path` does not exist or is neither,
when the path of a socket does not fit into a socket address, or when any step fails.

**`ReleaseWormHole`** unmaps the region, closes every descriptor the hole holds and
frees it. Accepts `NULL`.

**`KickWormHole`** rings the doorbell of the peer `identifier`. Returns `0` or `-EBADF`:

| Mode | `0` | `-EBADF` |
|------|-----|----------|
| Guest | Always: the ID is written to the doorbell register | — |
| Host | The value `1` has been written to the eventfd of the peer | ID above `65535`, no eventfd for that ID, or the write failed |
| Any | — | `hole` is `NULL` |

**`HandleWormHoleServerEvent`** reads every message waiting on `host.socket`
without blocking. A message carrying an ID and a descriptor is the eventfd of a peer:
the first one per ID is kept, later ones and those of an ID outside `0…65535` are
closed, and the one of this peer's own ID is also duplicated into `handle`. A message
carrying an ID and no descriptor says that the peer has gone: its eventfd is closed.
Returns `1` once the socket is empty, `0` when the server has closed the connection,
and `-1` with `errno` set on an error, having reported `HOLE_EVENT_LOST` with
`HOLE_LOST_SERVER` in the last two cases; an interrupted read is repeated. For a guest hole
it reads nothing, reports nothing and returns `0`.

**`HandleWormHoleBellEvent`** is called when `handle` is readable or has been read.
With `HOLE_CLIENT_VALUE_READ` it reads `size` bytes from `handle` itself, and returns
without an event when nothing can be read: the source has not been granted yet or has
nothing to give. Any other value is taken as already read and is not looked at. Then it
calls the function with `HOLE_EVENT_BELL`.

**`HandleWormHoleLossEvent`** calls the function with `HOLE_EVENT_LOST` and `value`.

## WormLoop

```c
struct WormLoop
{
  struct WormHole* hole;
  struct FastRing* ring;
  struct FastRingDescriptor* source;  // Watch of event source delivering the bell
  struct FastRingDescriptor* server;  // Watch of connection to ivshmem-server (host mode only)
};

struct WormLoop* CreateWormLoop(struct FastRing* ring, struct WormHole* hole);
void ReleaseWormLoop(struct WormLoop* loop);
```

**`CreateWormLoop`** watches the sources of a hole on a ring of **FastRing**. In guest
mode it watches `handle` at once. In host mode it watches `host.socket`, and the watch of
`handle` is added once the server has granted it. The kick is left as it is. Returns
`NULL` when `ring`
or `hole` is `NULL`, or when the loop cannot be allocated or its first watch added.

**`ReleaseWormLoop`** removes both watches and frees the loop. Accepts `NULL`.
