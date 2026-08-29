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
else is concerned, and `UpdateWormArena` gives back what it was holding:

* the bits and counts of the holders it had, and the blocks that are left with none,
* the locks it was holding as a writer,
* the ownership of the slots it has taken in a queue and not filled in, so that the
  consumer that gets there next takes them back.

The sweep also runs before a slot is handed out again, so a number never stands for
two peers at once: whatever the peer that had it was holding is released while the
number still means that very peer.

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

`UpdateWormArena` sweeps whatever the peers that are gone have left behind and
rebuilds the maps of the entities and of the named blocks. It has to be called for a
block created by another peer to be found by name, and it is what a peer calls when
the doorbell tells it to look around.

Every `timeout` is in milliseconds. Creating anything and registering an entity wait
for the lock of the arena, while `LockWormArenaDescriptor` and `LockWormArenaRegion`
wait for the lock they are about to take. Releasing waits for nothing at all: giving
a block back is a matter of exchanges of its own, and a block lost to a timeout would
be lost for good.

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
block since it last looked: zero if nothing has, one if it has, and the callback of
the descriptor is called in that case.

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

* The doorbell is not wired to `UpdateWormArena`, so a peer notices the changes of
  its neighbours only when it asks.
* `HandleWormArenaEntityChange` is declared and never called.
* Collections can not be deleted, and the area of a deleted one would not be reused.
* Registration does not check whether the name is taken by somebody alive.
