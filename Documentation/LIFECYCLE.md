# Lifecycle

This document describes the life of a `WormHole` and of a `WormLoop` over it: who owns
them, how a hole comes up in each mode, where its bells come from, when a kick reaches
a peer, what is reported when a peer or the server goes, and how everything is
released. The functions are listed in [API.md](API.md), the overall model in the
[README](../README.md).

- [Threads and Ownership](#threads-and-ownership)
- [Coming Up](#coming-up)
- [Peers Coming and Going](#peers-coming-and-going)
- [Bells](#bells)
- [Kicks](#kicks)
- [Under a WormLoop](#under-a-wormloop)
- [Losses](#losses)
- [Release](#release)

## Threads and Ownership

`WormHole` and `WormLoop` take no locks and start no threads. A hole is owned by the
caller of `CreateWormHole`, who releases it with `ReleaseWormHole`. Nothing inside it is
synchronized: the table of eventfds that `HandleWormHoleHostServerEvent` changes is the
one the kick reads, so the calls on one hole are made from one thread or serialized by
the caller.

A `WormLoop` and a `WormArena` refer to a hole and never release it. A loop is owned by
the caller of `CreateWormLoop` and released with `ReleaseWormLoop`.

## Coming Up

**Guest mode.** `CreateWormHole` opens the UIO device and maps the region and the
registers. Everything the hole needs is there once it returns: the ID is read from the
device, and `handle` is the device itself, so a bell can be heard from the start. A
guest has no connection to a server and is never told which other peers there are.

**Host mode.** `CreateWormHole` connects to *ivshmem-server* and reads the first three
messages while still inside the call: the version of the protocol, the ID of this peer
and the descriptor of the shared object, which it maps. What the server sends after that
— the eventfds of the peers already there and then those of this peer itself — is left
on the socket. Until `HandleWormHoleHostServerEvent` has read it, `handle` is `-1`, no
bell can be heard and no peer can be kicked. A host client is therefore not ready when
`CreateWormHole` returns but after its socket has been read for the first time.

## Peers Coming and Going

Only a host hole learns about peers, and only from `HandleWormHoleHostServerEvent`.

- A peer that connects is announced with its eventfds. The first one is kept in the
  table under its ID, and kicks to that ID reach it from then on. The server sends one
  eventfd per vector; the rest are closed, so only vector 0 is used.
- The announcement of this peer's own ID also gives the hole its event source: the
  eventfd is kept in the table, so that the peer can kick itself, and a duplicate of it
  becomes `handle`.
- A peer that disconnects is announced with its ID alone. Its eventfd is closed and
  kicks to that ID return `-EBADF` from then on.

No callback is called for a peer coming or going. Whether the process behind a peer is
still alive is not something a hole knows either; in an arena that is what the lease is
for (see [Lifetime](ARENA.md#lifetime)).

## Bells

A bell comes from `handle`: the UIO device in guest mode, the eventfd of the peer itself
in host mode. The caller waits for it to become readable and calls
`HandleWormHoleInterruptEvent`, either with the value it has read or with
`HOLE_CLIENT_VALUE_READ` to have the source read there. The bell is called after that,
in the thread that called `HandleWormHoleInterruptEvent`.

A bell names neither the peer that rang nor how many times it did. A read of the source
wakes for all the kicks since the previous one, so several kicks may arrive as one bell,
and the value read is not handed to the callback. A bell says that somebody has rung and
no more; what has changed is for the callback to find out in the memory.

In guest mode every process that holds the device is woken by an interrupt, not only the
first of them, and all of them share the ID of the machine.

## Kicks

`KickWormHole` may be called at any time on a live hole, and its result says whether the
kick went anywhere.

- **Guest mode.** The ID is written to the doorbell register and the result is always
  `0`, whether a peer of that ID exists or not.
- **Host mode.** The value `1` is written to the eventfd of the peer, and the result is
  `0`. It is `-EBADF` before the server has announced the peer, after it has announced
  its departure, for an ID above `65535`, which is as large as the table of peers goes,
  and when the write fails.

A peer may kick itself in both modes. `WormArena` rings peers through `KickWormHole`, so
all of the above holds for `KickWormArenaEntity` and `KickAllWormArenaEntities` as well.

## Under a WormLoop

`CreateWormLoop` moves the waiting into a ring of **FastRing**, and with it the calls that
the caller would otherwise make.

- **Guest mode.** The loop watches `handle` from the start. The kick is left as it is.
- **Host mode.** The loop watches `host.socket` and calls
  `HandleWormHoleHostServerEvent` whenever it is readable. Once that call has produced
  `handle`, the loop adds a watch of it. The kick is replaced by an io_uring write:
  `0` then means that the write has been handed to the ring, not that it has been done;
  the ring submits it on its next iteration and nobody sees its result.

When `handle` becomes readable the loop calls `HandleWormHoleInterruptEvent` with
`HOLE_CLIENT_VALUE_READ`, so the bell runs inside `WaitForFastRing()` on the thread of
the ring, as every watch callback of FastRing does. It has nothing to wait for there.

## Losses

A loss is the end of a source: the connection to the server, or the source of bells.
It is reported through the `loss` field of the hole, which the caller assigns after
`CreateWormHole`, and `HandleWormHoleLossEvent`, which calls it.

**Without a loop** nothing is called on its own. `HandleWormHoleHostServerEvent` returns
`1` once everything waiting has been read, `0` when the server has closed the connection
and `-1` on an error, and what to make of the last two is up to the caller, including
whether to call `HandleWormHoleLossEvent`.

**Under a loop** the loss is called on the thread of the ring:

- when the watch of `host.socket` reports an error, a hangup or an invalid descriptor,
  when `HandleWormHoleHostServerEvent` returns `0` or less, or when the watch of `handle`
  cannot be added. The watch of the socket is removed first.
- when the watch of `handle` reports an error, a hangup or an invalid descriptor. That
  watch is removed first. In host mode, a later message of the server puts a new watch on
  `handle`.

Nothing reconnects. A restarted server hands out a new region, so a client that wants to
go on creates a new hole (see [Instances](SERVER.md#instances)).

## Release

Everything is released in the reverse order of creation:

1. `ReleaseWormLoop`, while the ring still exists, since it removes its watches from it.
   In host mode it also takes its kick away and leaves none in its place: from then on
   `KickWormHole` on that hole returns `-EBADF`. In guest mode the kick is untouched.
2. `ReleaseFastRing`.
3. `ReleaseWormArena`, while the hole still exists, since the arena lives in its memory.
   An arena that outlives its loop goes on working on the memory, but in host mode its
   kicks return `-EBADF`.
4. `ReleaseWormHole`, which unmaps the region, closes every descriptor the hole holds and
   frees it. Pointers into `memory` are invalid afterwards. In host mode the closed socket
   is how the server learns that the peer has gone and tells the others.

`ReleaseWormLoop` and `ReleaseWormHole` accept `NULL`.
