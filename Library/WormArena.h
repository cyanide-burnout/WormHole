#ifndef WORMARENA_H
#define WORMARENA_H

#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>

#include "WormHole.h"
#include "HashMap.h"

#ifndef __cplusplus

#include <stdatomic.h>

#define ATOMIC(type)  type _Atomic

#else

#include <atomic>
#include <memory>

#define ATOMIC(type)  std::atomic<type>

#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define WORM_LOCK_READ    1
#define WORM_LOCK_WRITE  -1

// Everything about a lock lives in a single word, so taking it, giving it back and
// taking it over from a peer that is gone are one exchange each and nothing has to
// be kept consistent with anything. The state is zero while the lock is free,
// WORM_LOCK_WRITE for a writer and a count of the readers otherwise. The generation
// is bumped by a takeover, so the peer being cut off can tell the lock is not its
// own any more. The owner is the entity of the writer, and it is the very number
// the lease is asked about, so nothing has to be published beside the word itself.
// The deadline is counted in tenths of a second and is compared by a signed
// difference, the way jiffies are, so its wrap is of no concern

#define WORM_LOCK(generation, owner, state, deadline)  (((uint64_t)(generation) << 48) | ((uint64_t)(uint8_t)(owner) << 40) | ((uint64_t)(uint8_t)(state) << 32) | (uint32_t)(deadline))
#define WORM_LOCK_GENERATION(value)                    ((uint16_t)((value) >> 48))
#define WORM_LOCK_OWNER(value)                         ((uint8_t)((value) >> 40))
#define WORM_LOCK_STATE(value)                         ((uint8_t)((value) >> 32))
#define WORM_LOCK_DEADLINE(value)                      ((uint32_t)((value) & 0xffffffff))
#define WORM_LOCK_EXPIRED(value, moment)               ((int32_t)((moment) - WORM_LOCK_DEADLINE(value)) > 0)

// Peer packs the identity of a machine together with the process ID on it, so that
// an owner can be claimed and compared by a single atomic operation. The identity is
// a hash of what the machine calls itself and not the peer ID of ivshmem: the latter
// is handed out anew on every connection and taken back when a peer leaves

#define WORM_PEER(identifier, process)  (((uint64_t)(identifier) << 32) | (uint32_t)(process))
#define WORM_PEER_IDENTIFIER(value)     ((uint32_t)((value) >> 32))
#define WORM_PEER_PROCESS(value)        ((int32_t)((value) & 0xffffffff))

// Shared memory objects

// The signature is the claim word of the layout: a fresh region reads as zero,
// the peer that wins the CAS lays the arena out and publishes the signature.
// The last character of it is the version of the layout and has to be bumped
// whenever anything in the structures below moves, so that a peer built against
// another one refuses the region instead of reading it as its own

#define WORM_ARENA_SIGNATURE  0x324c4f484d524f57ULL  // WORMHOL2

// Entities are a fixed part of the magic: their count is what the map of holders
// in a block can address, and the slot number is the identity everywhere else.
// Slot zero is never handed out, its bit belongs to the queues

// One of these is what the map of the named blocks is rebuilt by, the other the arena
// does not read at all: it is there for an application that wants to notice a
// neighbour coming or going by a single load instead of walking the slots

#define WORM_REVISION_ENTITY      0
#define WORM_REVISION_COLLECTION  1
#define WORM_REVISION_COUNT       2

#define WORM_ARENA_ENTITIES     64
#define WORM_ARENA_COLLECTIONS  128

// The round of work of an entity and its state live in one word, so that whoever
// looks at a neighbour reads the two of them together and never the state of one
// round beside the number of another. The round is bumped by every touch of the
// lease and counts the passes of work a peer has made; the state says whether it is
// still to be addressed. An entity that is quiescing is alive and holds what it
// held, it is only not to be given anything new

#define WORM_ENTITY(round, state)   (((uint64_t)(round) << 8) | ((state) & 0xff))
#define WORM_ENTITY_ROUND(value)    ((value) >> 8)
#define WORM_ENTITY_STATE(value)    ((uint32_t)((value) & 0xff))
#define WORM_ENTITY_ROUND_STEP      WORM_ENTITY(1, 0)

#define WORM_ENTITY_QUIESCING       (1U << 0)
#define WORM_ROUND_NONE             UINT64_MAX

// Kind of the collection: a pool keeps blocks, a queue keeps references to them,
// a region is a named piece of memory the arena knows the bounds of and nothing else

#define WORM_COLLECTION_POOL    1
#define WORM_COLLECTION_QUEUE   2
#define WORM_COLLECTION_REGION  3

// A block belongs to everybody whose bit is set. Bit zero says it is held by a
// queue, the rest are the entity slots, so a block whose holders are all gone
// can be told from a block that is really in use. Every holder has a count of
// its own holds beside the map, and its bit stands for that count being non-zero,
// the queues included: theirs is the count of the slots the block is waiting in

#define WORM_HOLDER_LIMIT      65535

#define WORM_HOLDER_QUEUE      1ULL
#define WORM_HOLDER(number)    (1ULL << (number))

// The free list of a pool is a stack of block numbers with the revision of the
// block in the upper half, so a block taken and given back in between is told
// apart from the one that has never moved

// A slot of a queue is a single word: the number of the collection in the upper
// half and the number of the block in the lower one, and zero means it is free.
// A reservation is told apart by the topmost bit alone, which a pair can never
// carry because a collection is numbered far below it. A consumer that runs into
// one asks the lease about the entity in it instead of waiting out a deadline

#define WORM_SLOT_EMPTY               0ULL
#define WORM_SLOT_LOCK                (1ULL << 63)
#define WORM_SLOT(collection, block)  (((uint64_t)(collection) << 32) | (uint32_t)(block))
#define WORM_SLOT_COLLECTION(value)   ((uint32_t)((value) >> 32))
#define WORM_SLOT_BLOCK(value)        ((uint32_t)((value) & 0xffffffff))

// A reservation carries the position it has been made for beside the entity that has
// made it. The number alone would repeat as soon as the ring came round and the slot
// of that entity got handed out again, and a producer waking up long after both would
// then find its own token in the reservation of somebody else. The position is what
// makes the token unrepeatable, so it is given everything the marker and the entity
// do not need: fifty seven bits, which a queue of a million a second walks through
// in some four thousand years

#define WORM_SLOT_RESERVE(position, entity)  (WORM_SLOT_LOCK | ((uint64_t)(position) << 6) | ((entity) & 0x3f))
#define WORM_SLOT_RESERVED(value)            ((value) & WORM_SLOT_LOCK)
#define WORM_SLOT_ENTITY(value)              ((uint32_t)((value) & 0x3f))
#define WORM_SLOT_DISOWNED(value)            ((value) & ~0x3fULL)

#define WORM_BLOCK_NONE               0xffffffff
#define WORM_BLOCK(tag, number)       (((uint64_t)(tag) << 32) | (uint32_t)(number))
#define WORM_BLOCK_NUMBER(value)      ((uint32_t)((value) & 0xffffffff))

struct WormArenaLock
{
  ATOMIC(uint64_t) value;                           // Generation, owner, state and deadline together, see WORM_LOCK
};

struct WormArenaEntity
{
  uint32_t number;                                  // Entity number, the same as the slot in the magic
  ATOMIC(uint32_t) address;                         // Peer ID of ivshmem, where the doorbell of this entity is rung
  ATOMIC(uint64_t) peer;                            // Identity of the machine and the process on it, see WORM_PEER
  ATOMIC(uint64_t) time;                            // Validity time, nanoseconds of CLOCK_REALTIME
  ATOMIC(uint64_t) state;                           // Round of work and state of the entity together, see WORM_ENTITY
  uint64_t reserved[4];                             // Room to grow into, has to be kept zero until it is taken
  char name[64];                                    // Client name
};

struct WormArenaBlock
{
  struct WormArenaLock lock;                        // Spin lock of the data
  ATOMIC(uint64_t) revision;                        // Revision mark, has to be changed when the data have been changed, doubles as the tag of the free list
  ATOMIC(uint64_t) holders;                         // Map of holders, the block is free while it is zero
  ATOMIC(uint64_t) next;                            // Next free block, see WORM_BLOCK
  uint64_t offset;                                  // Offset of data, set once the pool has been created
  uint64_t size;                                    // Size of the data in use, up to the length of the pool
  uint64_t reserved[2];                             // Room to grow into, has to be kept zero until it is taken
  char name[64];                                    // Block name, empty while the block carries a message
  ATOMIC(uint16_t) counts[WORM_ARENA_ENTITIES];     // Count of the holds of every entity, the map above is the summary of them
};

struct WormArenaCollection
{
  struct WormArenaLock lock;                        // Region: spin lock of the data
  ATOMIC(uint64_t) revision;                        // Revision mark, has to be changed when the data have been changed
  ATOMIC(uint64_t) head;                            // Pool: first free block, see WORM_BLOCK. Queue: position to take from
  ATOMIC(uint64_t) tail;                            // Queue: position to put to
  ATOMIC(uint32_t) type;                            // One of WORM_COLLECTION_*, zero while the slot is free
  uint32_t filler;                                  //
  uint64_t count;                                   // Capacity of the table in entries
  uint64_t length;                                  // Pool: size of the data of a single block
  uint64_t offset;                                  // Offset of the whole region of the collection, the table comes first
  uint64_t size;                                    // Size of that region
  uint64_t reserved[7];                             // Room to grow into, has to be kept zero until it is taken
  char name[64];                                    // Collection name
};

struct WormArenaMagic
{
  ATOMIC(uint64_t) signature;                       // WORM_ARENA_SIGNATURE once the arena has been laid out
  ATOMIC(uint64_t) revisions[WORM_REVISION_COUNT];  // Revisions of the entities and of the collections, see WORM_REVISION_*
  struct WormArenaLock lock;                        // Global spin lock
  uint64_t reserved[4];                             // Room to grow into, has to be kept zero until it is taken
  char name[64];                                    // Arena name, to tell the region of the application from a foreign one
  struct WormArenaEntity entities[WORM_ARENA_ENTITIES];
  struct WormArenaCollection collections[WORM_ARENA_COLLECTIONS];
};

// Local objects

struct WormArena
{
  pthread_rwlock_t lock;                            //
  struct WormHole* hole;                            //
  struct HashMap* blocks;                           //
  struct WormArenaEntity* entity;                   // Local entity
  uint64_t revision;                                // Revision of the collections the map has been built of
  uint32_t host;                                    // Identity of this machine, see WORM_PEER
  pid_t process;                                    // Local Process ID

  // Where the rounds of the neighbours stood when this peer began to quiesce. A slot
  // holding WORM_ROUND_NONE is one to pay no attention to: nobody was there, or has
  // come along since and has therefore never addressed this peer as a taking one

  uint64_t rounds[WORM_ARENA_ENTITIES];
};

struct WormArenaDescriptor
{
  struct WormArena* arena;                          //
  struct WormArenaBlock* block;                     //
  uint64_t revision;                                // Revision of the block as of the last look at it
  uint32_t generation;                              // Generation of the lock while it is held
  uint32_t state;                                   // WORM_LOCK_READ or WORM_LOCK_WRITE while locked, zero otherwise
  uint8_t* data;                                    // Pointer to the data of the block
};

struct WormArena* CreateWormArena(struct WormHole* hole, const char* name);
void ReleaseWormArena(struct WormArena* arena);

int RegisterWormArenaEntity(struct WormArena* arena, const char* name, int valid, int timeout);
int UpdateWormArena(struct WormArena* arena, int timeout);
int TouchWormArena(struct WormArena* arena, int valid, int timeout);

int QuiesceWormArena(struct WormArena* arena, int timeout);
int CheckWormArenaQuiescence(struct WormArena* arena, int timeout);

struct WormArenaEntity* FindWormArenaEntity(struct WormArena* arena, const char* name);
int KickWormArenaEntity(struct WormArena* arena, struct WormArenaEntity* entity);
int KickAllWormArenaEntities(struct WormArena* arena);

struct WormArenaCollection* CreateWormArenaPool(struct WormArena* arena, const char* name, uint32_t blocks, uint64_t size, int timeout);
struct WormArenaCollection* CreateWormArenaQueue(struct WormArena* arena, const char* name, uint32_t slots, int timeout);
struct WormArenaCollection* CreateWormArenaRegion(struct WormArena* arena, const char* name, uint64_t size, int timeout);
struct WormArenaCollection* FindWormArenaCollection(struct WormArena* arena, const char* name, uint32_t type);

struct WormArenaDescriptor* CreateWormArenaDescriptor(struct WormArena* arena, struct WormArenaCollection* pool, const char* name, uint64_t size, int timeout);
struct WormArenaDescriptor* AttachWormArenaDescriptor(struct WormArena* arena, struct WormArenaBlock* block);
struct WormArenaDescriptor* OpenWormArenaDescriptor(struct WormArena* arena, const char* name);
void ReleaseWormArenaDescriptor(struct WormArenaDescriptor* descriptor);

int LockWormArenaDescriptor(struct WormArenaDescriptor* descriptor, int type, int timeout);
void UnlockWormArenaDescriptor(struct WormArenaDescriptor* descriptor);
int CheckWormArenaDescriptor(struct WormArenaDescriptor* descriptor);

int PushWormArenaQueue(struct WormArena* arena, struct WormArenaCollection* queue, struct WormArenaDescriptor* descriptor);
struct WormArenaDescriptor* PopWormArenaQueue(struct WormArena* arena, struct WormArenaCollection* queue);

int LockWormArenaRegion(struct WormArena* arena, struct WormArenaCollection* region, int type, int timeout, uint32_t* generation);
void UnlockWormArenaRegion(struct WormArenaCollection* region, int type, uint32_t generation);

static inline __attribute__((always_inline)) uint8_t* GetWormArenaRegionData(struct WormArena* arena, struct WormArenaCollection* region)
{
  return arena->hole->memory + region->offset;
}

#ifdef __cplusplus
}
#endif

#endif
