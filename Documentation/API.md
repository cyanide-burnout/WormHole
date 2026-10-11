# API

This document lists the interface of `WormHole` (`Library/WormHole.h`) and `WormLoop`
(`Library/WormLoop.h`): the functions, the fields of a hole that the caller uses, the
constants and the callbacks. When each of them may be called and what happens around
them is in [LIFECYCLE.md](LIFECYCLE.md); the interface of `WormArena` is in
[ARENA.md](ARENA.md#interface), the overall model in the [README](../README.md).

- [WormHole](#wormhole)
  - [Constants](#constants)
  - [Callbacks](#callbacks)
  - [Structure](#structure)
  - [Functions](#functions)
- [WormLoop](#wormloop)

## WormHole

```c
struct WormHole* CreateWormHole(const char* path, HandleWormHoleBellFunction function, void* closure);
void ReleaseWormHole(struct WormHole* hole);

int KickWormHole(struct WormHole* hole, uint32_t identifier);

int HandleWormHoleHostServerEvent(struct WormHole* hole);
void HandleWormHoleInterruptEvent(struct WormHole* hole, int64_t value);
void HandleWormHoleLossEvent(struct WormHole* hole);
```

### Constants

| Name | Value | Meaning |
|------|-------|---------|
| `HOLE_CLIENT_TYPE_GUEST` | `1` | `type` of a hole opened through the UIO driver |
| `HOLE_CLIENT_TYPE_HOST` | `2` | `type` of a hole connected to *ivshmem-server* |
| `HOLE_CLIENT_VALUE_READ` | `INT64_MIN` | Passed to `HandleWormHoleInterruptEvent` to have the event source read by it |

### Callbacks

```c
typedef void (*HandleWormHoleBellFunction)(struct WormHole* hole);
typedef void (*HandleWormHoleLossFunction)(struct WormHole* hole);
```

The bell is given to `CreateWormHole` and is called by `HandleWormHoleInterruptEvent`.
The loss is not a parameter of anything: the caller assigns the `loss` field of the
hole, and `HandleWormHoleLossEvent` calls it. Either may be `NULL`, and is then not
called. Both are given the hole alone; what they need is reached through `closure`.

`ReleaseWormHoleFunction` and `KickWormHoleFunction` are the types of the `release` and
`kick` fields, which the mode fills in and `WormLoop` replaces; the caller goes through
`ReleaseWormHole` and `KickWormHole` instead.

### Structure

The fields the caller reads or sets:

| Field | Type | Meaning |
|-------|------|---------|
| `type` | `int` | `HOLE_CLIENT_TYPE_GUEST` or `HOLE_CLIENT_TYPE_HOST` |
| `closure` | `void*` | The `closure` given to `CreateWormHole`, for the callbacks |
| `loss` | `HandleWormHoleLossFunction` | Called by `HandleWormHoleLossEvent`; `NULL` after creation |
| `identifier` | `uint64_t` | ID of this peer |
| `memory` | `uint8_t*` | The shared region |
| `length` | `size_t` | Size of the shared region |
| `handle` | `int` | Event source that delivers the bell; `-1` in host mode until the server has granted it |
| `size` | `size_t` | How much to read from `handle`: 4 bytes in guest mode, 8 in host mode |
| `host.socket` | `int` | Connection to *ivshmem-server*, host mode only |

The rest belongs to the library: `kick`, `release` and `bell`; `extension`, which a
`WormLoop` points at itself in host mode; `host.memory`, the descriptor of the shared
object; `host.handles`, the eventfds of all peers by ID, `-1` where there is none; and
`guest.registers`, the mapping of BAR0 of the device.

### Functions

**`CreateWormHole`** picks the mode by what `path` is. A character device is taken for
the UIO device and gives a guest hole: the device is opened, its name in sysfs has to be
`uio_ivshmem`, map 1 is mapped as the region and map 0 as the registers, and the ID is
read from the `IVPosition` register. A socket is taken for *ivshmem-server* and gives a
host hole: the client connects, expects protocol version `0`, receives its ID and the
descriptor of the shared object, and maps the whole object. `function` is the bell and
`closure` is stored in the hole. Returns `NULL` when `path` does not exist or is neither,
or when any step fails.

**`ReleaseWormHole`** unmaps the region, closes every descriptor the hole holds and
frees it. Accepts `NULL`.

**`KickWormHole`** rings the doorbell of the peer `identifier`. Returns `0` or `-EBADF`:

| Mode | `0` | `-EBADF` |
|------|-----|----------|
| Guest | Always: the ID is written to the doorbell register | — |
| Host | The value `1` has been written to the eventfd of the peer | No eventfd for that ID, or the write failed |
| Host under a `WormLoop` | A write to the eventfd has been handed to the ring | ID above `65535`, no eventfd for that ID, or no descriptor available |
| Any | — | `hole` is `NULL`, or it has no kick (after `ReleaseWormLoop` in host mode) |

The plain host kick does not check the range of `identifier`; it has to be within
`0…65535`.

**`HandleWormHoleHostServerEvent`** reads every message waiting on `host.socket`
without blocking. A message carrying an ID and a descriptor is the eventfd of a peer:
the first one per ID is kept, later ones and those of an ID outside `0…65535` are
closed, and the one of this peer's own ID is also duplicated into `handle`. A message
carrying an ID and no descriptor says that the peer has gone: its eventfd is closed.
Returns `1` once the socket is empty, `0` when the server has closed the connection,
and `-1` with `errno` set on an error; an interrupted read is repeated. For a guest hole
it reads nothing and returns `0`.

**`HandleWormHoleInterruptEvent`** is called when `handle` is readable or has been read.
With `HOLE_CLIENT_VALUE_READ` it reads `size` bytes from `handle` itself; any other
value is taken as already read and is not looked at. Then it calls the bell.

**`HandleWormHoleLossEvent`** calls `loss`, if set.

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
`handle` is added once the server has granted it; it also points `extension` at the loop
and replaces the kick with one that writes through the ring. Returns `NULL` when `ring`
or `hole` is `NULL`, or when the loop cannot be allocated or its first watch added.

**`ReleaseWormLoop`** removes both watches and frees the loop. In host mode it also takes
its kick away from the hole and leaves none in its place. Accepts `NULL`.
