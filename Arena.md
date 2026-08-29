# WormArena

An object store laid out inside the shared memory of a `WormHole`, meant to be used
by several processes on several virtual machines at once.

Nothing in it can rely on the primitives of a single kernel: futexes, and with them
every blocking primitive of the system, do not cross the boundary of a virtual
machine. What is left is atomic operations over the memory itself and the doorbell
of ivshmem. Everything below is built out of those two, which is why the shape of
the API is what it is: every participant announces itself and keeps a **lease**, and
anything a participant was holding when it disappeared is given back by a **sweep**
rather than by a destructor that will never run.

## Model

**Entity** — a registered participant, one per process. It carries the identity of
the peer, a name and a lease. The number of its slot is the identity used everywhere
else: in the map of the holders of a block and in the owner of a lock.

**Collection** — a named area of the arena, of one of three kinds:

* **pool** — a table of blocks of a fixed size with a list of free ones,
* **queue** — a table of slots, each naming a block of some pool,
* **region** — a piece of memory the arena knows the bounds of and nothing else.

**Block** — a unit of a pool. It belongs to everybody whose bit is set in its map of
holders, and every holder keeps a count of its own holds. A block with no holders is
back in the free list of its pool.

**Descriptor** — the local handle of a block. It holds a reference, caches the
pointer to the data and remembers the revision the block had when it was last looked
at.

## Lifetime

A peer keeps its entity alive by calling `TouchWormArena` more often than the
validity it asked for. Once a lease runs out the entity is gone as far as everybody
else is concerned, and a sweep gives back what it was holding:

* the bits and counts of the holders it had, and the blocks that are left with none,
* the locks it was holding as a writer,
* the ownership of the slots it has taken in a queue and not filled in, so that the
  consumer that gets there next takes them back.

Nobody calls the sweep on purpose. It runs where its price is worth paying and where
somebody is there to pay it: when an allocation has found the pool empty, and before
an entity slot is handed out again. The first is the only moment when reclaiming
memory is worth anything, and the one who has run out is the one who pays. The second
is a matter of correctness rather than of memory: a number never stands for two peers
at once, because whatever the peer that had it was holding is released while the
number still means that very peer.

## Time

WormArena does not synchronize the clocks of the machines. Leases and lock
deadlines are absolute times of `CLOCK_REALTIME`, so the clocks of all participants
have to agree within the error the application can tolerate. A clock ahead can take
a live peer for one that is gone, and a clock behind can keep the resources of a
dead peer longer than intended.

How close is close enough follows from two numbers: the validity an entity asks for
in `RegisterWormArenaEntity`, and the five seconds a lock may go untouched before it
may be taken over. The clocks have to agree well within the smaller of the two,
because a disagreement of that order is indistinguishable from a peer that has died
holding something.

Synchronize the hosts independently of the arena. NTP is sufficient for ordinary
leases; use PTP where tighter and more predictable agreement is required. When the
participants are KVM guests, the preferred way to carry the time of a host into
them is its virtual PTP clock: the `ptp_kvm` driver exposes it as a PHC such as
`/dev/ptp0`, and `chronyd` or `phc2sys` disciplines the guest `CLOCK_REALTIME` from
that clock. `kvm-clock` alone is a clocksource, not a wall-clock synchronization
service.

Guests of one host can all follow that host this way without exchanging time over
the network. Guests of different physical hosts still depend on those hosts being
synchronized with one another; virtual PTP then carries the time of each host into
its guests. Clock synchronization is part of the deployment, not of the
shared-memory protocol.

## Revisions

Three revisions answer three different questions, and none of them answers more than
one:

| | |
|---|---|
| `WormArenaBlock.revision` | the data of this block have changed |
| `WormArenaCollection.revision` | the named blocks of this pool have changed |
| `WormArenaMagic.revisions` | the entities, or the collections, of the arena have changed |

The one of a block is what `CheckWormArenaDescriptor` reads, and it is moved by the
release of a write lock. The one of the collections is what the map of the named
blocks is rebuilt by. The one of the entities the arena itself does not read at all:
it is there for an application that wants to notice a neighbour registering or leaving
by a single load instead of walking the slots.

It says nothing of a peer that has died, though. A lease ends with time and moves
nothing at all, so a departure nobody announced is not an event: it is seen by
walking the slots, or not seen until somebody needs what that peer was holding.

Only one thing is indexed, and it is the only one whose count is not bounded by a
constant: sixty three entities and a hundred and twenty seven collections are walked,
the named blocks of a pool are not.

**A block with no name moves no revision at all.** Only a named block is ever looked
for by name, so only a named block changes a set anybody searches. A block taken for
a message is nameless, lives as long as the message does, and its coming and going
costs the neighbours nothing — not a rebuild, not even a write into memory that would
have to be carried between the machines. Pushing into a queue counts nothing either,
for the same reason: a slot carries a block that has no name.

This is what makes the traffic of a queue free of any bookkeeping, and it is easy to
undo by adding a counter that looks symmetric.

## Locks

`WORM_LOCK_READ` and `WORM_LOCK_WRITE` are the two kinds. A lock is a single word
carrying the generation, the owner, the state and a deadline, so taking it, giving it
back and taking it over are one exchange each.

A lock whose deadline has run out is taken over from its owner if the lease of that
owner is over, and the generation is bumped by the very exchange that frees it. A
holder that has been cut off in this way finds the generation changed and releases
nothing, so it can not undo what somebody else has done in the meantime.

Blocks are locked through their descriptor, regions directly. Pools and queues have
no lock of their own: the tables of a pool are lists of exchanges, and a queue is
lock-free by design.

## Interface

### Arena

```c
struct WormArena* CreateWormArena(struct WormHole* hole, const char* name);
void ReleaseWormArena(struct WormArena* arena);

int RegisterWormArenaEntity(struct WormArena* arena, const char* name, int valid, int timeout);
int TouchWormArena(struct WormArena* arena, int valid, int timeout);
int UpdateWormArena(struct WormArena* arena, int timeout);
```

`CreateWormArena` lays the arena out if the region is untouched and attaches to it
otherwise. The name is the name of the arena itself and tells the region of one
application from the region of another. A region that carries neither zero nor a
signature of this version of the layout is refused at once.

`RegisterWormArenaEntity` takes a slot and publishes a lease of `valid`
milliseconds. It can be called once per arena.

`UpdateWormArena` rebuilds the map the named blocks are looked up through. It has to
be called for a block of another peer to be found by name, and it costs a comparison
when nothing has changed. Entities and collections are walked and not indexed, so
they need none of this.

Every `timeout` is in milliseconds. Creating anything and registering an entity wait
for the lock of the arena, while `LockWormArenaDescriptor` and `LockWormArenaRegion`
wait for the lock they are about to take. Releasing waits for nothing at all: giving
a block back is a matter of exchanges of its own, and a block lost to a timeout would
be lost for good.

### Entities and the doorbell

```c
struct WormArenaEntity* FindWormArenaEntity(struct WormArena* arena, const char* name);
int KickWormArenaEntity(struct WormArena* arena, struct WormArenaEntity* entity);
int KickAllWormArenaEntities(struct WormArena* arena);
```

`FindWormArenaEntity` walks the slots and needs nothing rebuilt beforehand. Sixty
three of them are a fraction of a microsecond, and a walk can not go stale the way an
index would: a lease ends with time and moves no revision, so nothing would rebuild
an index at that moment.

`KickWormArenaEntity` asks about no lease at all. A bell says no more than look
around, so ringing one that has just gone costs nothing, and neither does ringing an
address handed over to somebody else in the meantime.

`KickAllWormArenaEntities` rings everybody but ourselves and returns how many were
rung. It rings every address once and not once per entity behind it: all the
processes of one guest share the single device of it and answer at one address, so
ringing per entity would wake that guest as many times as it has processes.

### Collections

```c
struct WormArenaCollection* CreateWormArenaPool(struct WormArena* arena, const char* name, uint32_t blocks, uint64_t size, int timeout);
struct WormArenaCollection* CreateWormArenaQueue(struct WormArena* arena, const char* name, uint32_t slots, int timeout);
struct WormArenaCollection* CreateWormArenaRegion(struct WormArena* arena, const char* name, uint64_t size, int timeout);
struct WormArenaCollection* FindWormArenaCollection(struct WormArena* arena, const char* name, uint32_t type);
```

All three creators are shared by name: a peer that asks for a collection somebody
else has already made gets that one, and the sizes it asked for are ignored. `size`
of a pool is the size of the data of a single block, rounded up to a cache line.
`slots` of a queue has to be a power of two.

Collections are never moved and never grow, so a pool that has run out has run out
for good.

### Blocks

```c
struct WormArenaDescriptor* CreateWormArenaDescriptor(struct WormArena* arena, struct WormArenaCollection* pool, const char* name, uint64_t size, int timeout);
struct WormArenaDescriptor* OpenWormArenaDescriptor(struct WormArena* arena, const char* name);
struct WormArenaDescriptor* AttachWormArenaDescriptor(struct WormArena* arena, struct WormArenaBlock* block);
void ReleaseWormArenaDescriptor(struct WormArenaDescriptor* descriptor);

int LockWormArenaDescriptor(struct WormArenaDescriptor* descriptor, int type, int timeout);
void UnlockWormArenaDescriptor(struct WormArenaDescriptor* descriptor);
int CheckWormArenaDescriptor(struct WormArenaDescriptor* descriptor);
```

`CreateWormArenaDescriptor` takes a block out of the pool. A block that carries a
message is created with no name and is never looked up by one; a block that stands
for an object is named and is found by `OpenWormArenaDescriptor`.

`UnlockWormArenaDescriptor` bumps the revision of the block after a write, and
`CheckWormArenaDescriptor` tells the caller whether anything has happened to the
block since it last looked: zero if nothing has, one if it has. It costs a single
load and takes no lock, so it is what a peer walks its objects with once the
doorbell has told it to look around.

Descriptors of one block within one process are counted, so two of them are
independent of each other.

### Queues

```c
int PushWormArenaQueue(struct WormArena* arena, struct WormArenaCollection* queue, struct WormArenaDescriptor* descriptor);
struct WormArenaDescriptor* PopWormArenaQueue(struct WormArena* arena, struct WormArenaCollection* queue);
```

A slot carries no data of its own, only the numbers of the collection and of the
block, so nothing is copied: the message is the block, and pushing it hands the
ownership over. The producer is free to release its own descriptor right after the
push. `-ENOBUFS` means the queue is full, `-EAGAIN` that another producer has taken
the very same slot and the push is worth trying again.

`-EOWNERDEAD` is a different matter. A producer takes a position, marks it with a
token of its own and only then puts the block in, by an exchange that has to find
that very token in place. Failing to find it means the lease of this peer has run out
while it was between the two — stopped by a signal, or on a machine that was paused —
and everything it was holding has already been given back on its behalf. The
descriptor it has in hand stands for nothing any more, the block behind it may well
belong to somebody else by now, and the only way on is to register again.

### Regions

```c
int LockWormArenaRegion(struct WormArena* arena, struct WormArenaCollection* region, int type, int timeout, uint32_t* generation);
void UnlockWormArenaRegion(struct WormArenaCollection* region, int type, uint32_t generation);

uint8_t* GetWormArenaRegionData(struct WormArena* arena, struct WormArenaCollection* region);
```

A region has no descriptor, so the generation of its lock is carried by the caller.
Whatever lives inside a region is the business of the application: the arena only
lays it out, zeroes it once and never looks into it again.

## Rules

* One entity per arena per process, registered once and kept alive by `TouchWormArena`.
* The lock of a block goes before its reference: unlock the descriptor, then release it.
* Nothing is to be waited for inside the callback of the doorbell: the loop is not
  running while the callback is.
* Numbers read out of the shared memory are the numbers a neighbour has written, so
  they are checked against the bounds of the region every time and never taken on
  trust.

## Limits

| | |
|---|---|
| Entities | 63, the slot zero is never handed out |
| Collections | 127, the slot zero is never handed out |
| Holds of one block by one entity | 65535 |
| Slots of a queue one block waits in | 65535 |
| Block | 256 bytes of description |
| Collection | 192 bytes of description |
| Entity | 128 bytes of description |
| Header of the arena | 32896 bytes |

Every table is aligned to a cache line and every entry of it is a multiple of one, so
no two of them are ever shared between peers.

## What is not done yet

* The doorbell is not wired to anything, so a peer looks around when it decides to.
  There is nothing of the arena to call on that path, though: a peer takes what has
  arrived out of its queues and asks `CheckWormArenaDescriptor` about the objects it
  holds, and neither of those touches a map.
* `HandleWormArenaEntityChange` is declared and never called.
* Collections can not be deleted, and the area of a deleted one would not be reused.
* Registration does not check whether the name is taken by somebody alive.
