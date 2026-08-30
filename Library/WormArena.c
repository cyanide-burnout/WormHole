#include "WormArena.h"
#include "CRC32C.h"

#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#define WORM_ARENA_CLAIM      0xffffffffffffffffULL
#define WORM_ARENA_ALIGNMENT  64
#define WORM_ARENA_TIMEOUT    1000
#define WORM_ARENA_HOLD       5000

_Static_assert(((uint64_t)WORM_ARENA_COLLECTIONS << 32) < WORM_SLOT_LOCK, "a pair of a slot must never carry the mark of a reservation");

#define ALIGN_OFFSET(value)  (((value) + WORM_ARENA_ALIGNMENT - 1) & ~((uint64_t)WORM_ARENA_ALIGNMENT - 1))

static uint32_t GetHostIdentifier()
{
  int handle;
  int length;
  uint8_t buffer[64];

  // The peer ID of ivshmem is handed out by the server anew on every connection and
  // is taken back when a peer leaves, so it says which connection this is rather than
  // which machine. The identity of the machine is what an entity has to carry to stay
  // the same across a reconnection and across a reboot: the arena outlives both, and
  // an object left by a machine is still the object of that very machine afterwards.
  //
  // Machines cloned off one image carry the machine ID of that image, and nothing
  // here can tell them apart, so an image is to be prepared with an identity of its own

  length = 0;
  handle = open("/etc/machine-id", O_RDONLY);

  if (handle >= 0)
  {
    length = read(handle, buffer, sizeof(buffer));
    close(handle);
  }

  if (length > 0)
  {
    //
    return GetCRC32C(buffer, length, 0);
  }

  // The host ID is a poor second: glibc makes it up out of the address of the machine
  // when there is no /etc/hostid, so it follows the network rather than the machine

  return (uint32_t)gethostid();
}

static uint64_t GetCurrentTime()
{
  struct timespec time;

  clock_gettime(CLOCK_REALTIME, &time);

  return (uint64_t)time.tv_sec * 1000000000ULL + (uint64_t)time.tv_nsec;
}

// Locks

static int IsEntityAlive(struct WormArena* arena, uint32_t number)
{
  struct WormArenaMagic* magic;

  if ((arena  == NULL) ||
      (number == 0)    ||
      (number >= WORM_ARENA_ENTITIES))
  {
    // The slot zero is never handed out, so it never belongs to anybody
    return 0;
  }

  magic = (struct WormArenaMagic*)arena->hole->memory;

  // Acquire for the same reason as everywhere else: the lease is what registration
  // publishes last, and whoever trusts it goes on to read the rest of the entity

  return atomic_load_explicit(&magic->entities[number].time, memory_order_acquire) >= GetCurrentTime();
}

static uint32_t GetCurrentMoment()
{
  // Tenths of a second are coarse enough for a lock held for a few of them and
  // leave the wrap of the deadline more than a decade away

  return (uint32_t)(GetCurrentTime() / 100000000ULL);
}

static void ResetSpinLock(struct WormArenaLock* lock)
{
  atomic_store_explicit(&lock->value, 0, memory_order_relaxed);
}

static void TakeOverSpinLock(struct WormArena* arena, struct WormArenaLock* lock, uint64_t value)
{
  if (IsEntityAlive(arena, WORM_LOCK_OWNER(value)))
  {
    // The lease is still valid, so the peer does exist and is merely slow
    return;
  }

  // Releasing the lock and bumping the generation is one exchange, and it is made
  // against the very word the deadline has been read from. A holder that has taken
  // the lock in the meantime has stamped its own deadline into that word, so this
  // fails and nothing is taken away from anybody alive. A stale owner read above
  // can not do any harm for the same reason

  atomic_compare_exchange_strong_explicit(&lock->value, &value, WORM_LOCK(WORM_LOCK_GENERATION(value) + 1, 0, 0, 0), memory_order_release, memory_order_relaxed);
}

static int SetSpinLock(struct WormArena* arena, struct WormArenaLock* lock, int type, int timeout, uint32_t* generation)
{
  uint8_t owner;
  uint8_t state;
  uint16_t mark;
  uint32_t moment;
  uint64_t value;
  uint64_t time;
  uint64_t limit;
  uint64_t expectation;

  limit = GetCurrentTime() + (uint64_t)timeout * 1000000ULL;

  do
  {
    value  = atomic_load_explicit(&lock->value, memory_order_relaxed);
    mark   = WORM_LOCK_GENERATION(value);
    state  = (type == WORM_LOCK_READ) * WORM_LOCK_STATE(value);
    moment = GetCurrentMoment();
    owner  = (type == WORM_LOCK_WRITE) * (arena->entity != NULL) * (arena->entity != NULL ? arena->entity->number : 0);

    // A writer expects the lock to be free, a reader takes it as it is unless a
    // writer holds it. The deadline is stamped by the very same exchange, so there
    // is no moment when the lock is held and says nothing about for how long

    expectation = WORM_LOCK(mark, WORM_LOCK_OWNER(value), state, WORM_LOCK_DEADLINE(value));

    if ((state != (uint8_t)WORM_LOCK_WRITE) &&
        (atomic_compare_exchange_weak_explicit(&lock->value, &expectation,
           WORM_LOCK(mark, owner, state + type, moment + WORM_ARENA_HOLD / 100), memory_order_acquire, memory_order_relaxed)))
    {
      // The owner is published by the very same exchange, so nothing of the writer
      // before us can be left behind to be taken for the holder of the lock

      *generation = mark;
      return 0;
    }

    time = GetCurrentTime();

    if ((WORM_LOCK_STATE(value) != 0) &&
        (WORM_LOCK_EXPIRED(value, moment)))
    {
      // Nobody has touched the lock for too long, it is time to find out whether the holder is still there
      TakeOverSpinLock(arena, lock, value);
    }
  }
  while (time < limit);

  return -ETIME;
}

static void ClearSpinLock(struct WormArenaLock* lock, int type, uint32_t generation)
{
  uint64_t value;

  value = atomic_load_explicit(&lock->value, memory_order_relaxed);

  do
  {
    if (WORM_LOCK_GENERATION(value) != generation)
    {
      // The lock has been taken over while we were holding it, it is not ours to release
      return;
    }
  }
  while (!atomic_compare_exchange_weak_explicit(&lock->value, &value,
           WORM_LOCK(generation, (WORM_LOCK_STATE(value) != (uint8_t)type) * WORM_LOCK_OWNER(value), WORM_LOCK_STATE(value) - type, WORM_LOCK_DEADLINE(value)),
           memory_order_release, memory_order_relaxed));
}

static int SetArenaLock(struct WormArena* arena, int type, int timeout, uint32_t* generation)
{
  struct WormArenaMagic* magic;

  magic = (struct WormArenaMagic*)arena->hole->memory;

  return SetSpinLock(arena, &magic->lock, type, timeout, generation);
}

static void ClearArenaLock(struct WormArena* arena, int type, uint32_t generation)
{
  struct WormArenaMagic* magic;

  magic = (struct WormArenaMagic*)arena->hole->memory;

  ClearSpinLock(&magic->lock, type, generation);
}

static int SetBlockLock(struct WormArena* arena, struct WormArenaBlock* block, int type, int timeout, uint32_t* generation)
{
  if ((arena == NULL) ||
      (block == NULL))
  {
    //
    return -EINVAL;
  }

  return SetSpinLock(arena, &block->lock, type, timeout, generation);
}

static void ClearBlockLock(struct WormArenaBlock* block, int type, uint32_t generation)
{
  if (block != NULL)
  {
    //
    ClearSpinLock(&block->lock, type, generation);
  }
}

int LockWormArenaRegion(struct WormArena* arena, struct WormArenaCollection* region, int type, int timeout, uint32_t* generation)
{
  if ((arena  == NULL) ||
      (region == NULL) ||
      (atomic_load_explicit(&region->type, memory_order_acquire) != WORM_COLLECTION_REGION))
  {
    // The tables of a pool are covered by the lock of the arena and a queue needs no lock at all
    return -EINVAL;
  }

  return SetSpinLock(arena, &region->lock, type, timeout, generation);
}

void UnlockWormArenaRegion(struct WormArenaCollection* region, int type, uint32_t generation)
{
  if (region != NULL)
  {
    //
    ClearSpinLock(&region->lock, type, generation);
  }
}

// Collections

static int RemoveAll(void* key, size_t size, void* data, void* argument1, void* argument2)
{
  return 1;
}

static int TrackCollectionUpdate(struct WormArena* arena, int timeout)
{
  int result;
  int length;
  uint32_t number;
  uint32_t generation;
  uint64_t count;
  uint64_t revision;
  struct WormHole* hole;
  struct WormArenaMagic* magic;
  struct WormArenaBlock* block;
  struct WormArenaCollection* collection;

  hole   = arena->hole;
  magic  = (struct WormArenaMagic*)hole->memory;
  result = SetArenaLock(arena, WORM_LOCK_READ, timeout, &generation);

  if (result == 0)
  {
    pthread_rwlock_wrlock(&arena->lock);

    // A block coming and going moves the revision of its pool and not this one, so
    // the traffic of a queue makes nobody rebuild anything

    revision = atomic_load_explicit(&magic->revisions[WORM_REVISION_COLLECTION], memory_order_acquire);

    if (arena->revision != revision)
    {
      IterateThroughHashMap(arena->blocks, RemoveAll, NULL, NULL);

      for (number = 0; number < WORM_ARENA_COLLECTIONS; number ++)
      {
        collection = magic->collections + number;

        if (atomic_load_explicit(&collection->type, memory_order_acquire) != WORM_COLLECTION_POOL)
        {
          //
          continue;
        }

        block = (struct WormArenaBlock*)(hole->memory + collection->offset);
        count = collection->count;

        while (count > 0)
        {
          length = strlen(block->name);

          if ((length != 0) &&
              (atomic_load_explicit(&block->holders, memory_order_relaxed) != 0))
          {
            // A block carrying a message stays nameless and is never looked up by one
            PutIntoHashMap(arena->blocks, block->name, length, block);
          }

          block ++;
          count --;
        }
      }
    }

    arena->revision = revision;

    pthread_rwlock_unlock(&arena->lock);
    ClearArenaLock(arena, WORM_LOCK_READ, generation);
  }

  return result;
}

struct WormArenaCollection* FindWormArenaCollection(struct WormArena* arena, const char* name, uint32_t type)
{
  uint32_t number;
  struct WormArenaMagic* magic;
  struct WormArenaCollection* collection;

  if ((arena == NULL) ||
      (name  == NULL))
  {
    //
    return NULL;
  }

  magic = (struct WormArenaMagic*)arena->hole->memory;
  for (number = 0; number < WORM_ARENA_COLLECTIONS; number ++)
  {
    collection = magic->collections + number;

    if ((atomic_load_explicit(&collection->type, memory_order_acquire) == type) &&
        (strncmp(collection->name, name, sizeof(collection->name)) == 0))
    {
      //
      return collection;
    }
  }

  return NULL;
}

struct WormArenaEntity* FindWormArenaEntity(struct WormArena* arena, const char* name)
{
  uint32_t number;
  uint64_t time;
  struct WormArenaMagic* magic;
  struct WormArenaEntity* entity;

  if ((arena == NULL) ||
      (name  == NULL))
  {
    //
    return NULL;
  }

  // Sixty three slots are walked rather than indexed on purpose. A map would save a
  // fraction of a microsecond on a lookup that happens when a peer settles in, and
  // would have to be told that a lease has run out, which happens with time and moves
  // no revision at all. What is walked can not go stale

  magic = (struct WormArenaMagic*)arena->hole->memory;
  time  = GetCurrentTime();

  for (number = 1; number < WORM_ARENA_ENTITIES; number ++)
  {
    entity = magic->entities + number;

    // The lease is what registration publishes last, so it is taken with acquire and
    // everything of the entity is read after it. Taken any other way, the name here
    // could be the one of a slot that is not filled in yet

    if ((atomic_load_explicit(&entity->time, memory_order_acquire) >= time) &&
        (strncmp(entity->name, name, sizeof(entity->name)) == 0))
    {
      //
      return entity;
    }
  }

  return NULL;
}

int KickWormArenaEntity(struct WormArena* arena, struct WormArenaEntity* entity)
{
  if ((arena  == NULL) ||
      (entity == NULL))
  {
    //
    return -EINVAL;
  }

  // The lease is not asked about here on purpose. A bell says no more than look
  // around, so ringing one that has just gone costs nothing, and neither does ringing
  // an address that has been handed over to somebody else in the meantime

  return KickWormHole(arena->hole, atomic_load_explicit(&entity->address, memory_order_relaxed));
}

int KickAllWormArenaEntities(struct WormArena* arena)
{
  int count;
  uint32_t number;
  uint32_t index;
  uint32_t address;
  uint64_t time;
  struct WormArenaMagic* magic;
  struct WormArenaEntity* entity;

  if (arena == NULL)
  {
    //
    return -EINVAL;
  }

  magic = (struct WormArenaMagic*)arena->hole->memory;
  time  = GetCurrentTime();
  count = 0;

  for (number = 1; number < WORM_ARENA_ENTITIES; number ++)
  {
    entity = magic->entities + number;

    if ((entity == arena->entity) ||
        (atomic_load_explicit(&entity->time, memory_order_acquire) < time))
    {
      // Ourselves, or somebody whose lease has run out and who is not there to hear it
      continue;
    }

    // Only after the lease, never before it: an address read out of a slot that is
    // still being filled in belongs to nobody

    address = atomic_load_explicit(&entity->address, memory_order_relaxed);

    // All the processes of one guest share the single device of it and answer at one
    // address, so an address already rung is skipped rather than rung once per entity.
    // Ourselves we skip here as well: we have not been rung either, and taking our own
    // entity for one that has heard it would leave a neighbour on our machine unrung

    for (index = 1; index < number; index ++)
    {
      if (((magic->entities + index) != arena->entity)                                          &&
          (atomic_load_explicit(&magic->entities[index].time, memory_order_acquire)    >= time) &&
          (atomic_load_explicit(&magic->entities[index].address, memory_order_relaxed) == address))
      {
        //
        break;
      }
    }

    if (index == number)
    {
      count += (KickWormHole(arena->hole, address) == 0);
    }
  }

  return count;
}

// Blocks

static struct WormArenaCollection* GetBlockPool(struct WormArena* arena, struct WormArenaBlock* block)
{
  uint32_t number;
  uint64_t offset;
  struct WormArenaMagic* magic;
  struct WormArenaCollection* collection;

  magic  = (struct WormArenaMagic*)arena->hole->memory;
  offset = (uint8_t*)block - arena->hole->memory;
  for (number = 0; number < WORM_ARENA_COLLECTIONS; number ++)
  {
    collection = magic->collections + number;

    if ((atomic_load_explicit(&collection->type, memory_order_acquire) == WORM_COLLECTION_POOL) &&
        (offset >=  collection->offset)                                                         &&
        (offset <  (collection->offset + collection->size)))
    {
      //
      return collection;
    }
  }

  return NULL;
}

static void PushFreeBlock(struct WormHole* hole, struct WormArenaCollection* pool, struct WormArenaBlock* block)
{
  uint64_t value;
  uint64_t next;
  uint32_t number;
  struct WormArenaBlock* table;

  // The revision doubles as the tag of the list: it grows on every move of the
  // block, so the same number coming back is never taken for the one that stayed

  table  = (struct WormArenaBlock*)(hole->memory + pool->offset);
  number = block - table;
  next   = WORM_BLOCK(atomic_fetch_add_explicit(&block->revision, 1, memory_order_relaxed) + 1, number);

  do
  {
    value = atomic_load_explicit(&pool->head, memory_order_relaxed);
    atomic_store_explicit(&block->next, value, memory_order_relaxed);
  }
  while (!atomic_compare_exchange_weak_explicit(&pool->head, &value, next, memory_order_release, memory_order_relaxed));
}

static struct WormArenaBlock* PopFreeBlock(struct WormHole* hole, struct WormArenaCollection* pool)
{
  uint64_t value;
  uint64_t next;
  uint32_t number;
  struct WormArenaBlock* table;
  struct WormArenaBlock* block;

  table = (struct WormArenaBlock*)(hole->memory + pool->offset);

  do
  {
    value  = atomic_load_explicit(&pool->head, memory_order_acquire);
    number = WORM_BLOCK_NUMBER(value);

    if (number >= pool->count)
    {
      // The list is empty, and a number out of the table is treated the same way
      return NULL;
    }

    block = table + number;
    next  = atomic_load_explicit(&block->next, memory_order_relaxed);
  }
  while (!atomic_compare_exchange_weak_explicit(&pool->head, &value, next, memory_order_acq_rel, memory_order_relaxed));

  return block;
}

static uint64_t GetFreeOffset(struct WormArenaMagic* magic)
{
  uint32_t number;
  uint64_t offset;
  uint64_t limit;
  struct WormArenaCollection* collection;

  // Everything the collections have taken so far lies behind the magic in one piece

  offset = ALIGN_OFFSET(sizeof(struct WormArenaMagic));
  for (number = 0; number < WORM_ARENA_COLLECTIONS; number ++)
  {
    collection = magic->collections + number;
    limit      = ALIGN_OFFSET(collection->offset + collection->size);

    if (limit > offset)
    {
      //
      offset = limit;
    }
  }

  return offset;
}

static struct WormArenaCollection* AddArenaCollection(struct WormArenaMagic* magic, const char* name, uint64_t offset, uint64_t size)
{
  uint32_t number;
  struct WormArenaCollection* collection;

  // A slot with no type is free, whether it has never been used or has been given
  // back. The very first one is never handed out: its number in a slot of a queue
  // would be indistinguishable from an empty slot

  for (number = 1; number < WORM_ARENA_COLLECTIONS; number ++)
  {
    collection = magic->collections + number;

    if (atomic_load_explicit(&collection->type, memory_order_relaxed) == 0)
    {
      memset(collection->name, 0, sizeof(collection->name));
      strncpy(collection->name, name, sizeof(collection->name) - 1);

      collection->offset = offset;
      collection->size   = size;

      ResetSpinLock(&collection->lock);
      atomic_store_explicit(&collection->revision, 1, memory_order_relaxed);

      return collection;
    }
  }

  return NULL;
}

static void PublishArenaCollection(struct WormArenaMagic* magic, struct WormArenaCollection* collection, uint32_t type)
{
  // The type is published last, it is what makes the collection a part of the arena

  atomic_store_explicit(&collection->type, type, memory_order_release);
  atomic_fetch_add_explicit(&magic->revisions[WORM_REVISION_COLLECTION], 1, memory_order_release);
}

struct WormArenaCollection* CreateWormArenaRegion(struct WormArena* arena, const char* name, uint64_t size, int timeout)
{
  uint32_t generation;
  uint64_t offset;
  struct WormHole* hole;
  struct WormArenaMagic* magic;
  struct WormArenaCollection* collection;

  if ((arena == NULL) ||
      (name  == NULL) ||
      (size  == 0)    ||
      (size  >  arena->hole->length))
  {
    // A size that big has nowhere to fit anyway, and rounding it up would wrap it to zero
    return NULL;
  }

  if (collection = FindWormArenaCollection(arena, name, WORM_COLLECTION_REGION))
  {
    // Somebody has created the region already, it is shared as it is
    return collection;
  }

  hole       = arena->hole;
  magic      = (struct WormArenaMagic*)hole->memory;
  collection = NULL;

  if (SetArenaLock(arena, WORM_LOCK_WRITE, timeout, &generation) == 0)
  {
    if ((collection = FindWormArenaCollection(arena, name, WORM_COLLECTION_REGION)) == NULL)
    {
      offset = GetFreeOffset(magic);
      size   = ALIGN_OFFSET(size);

      if ((offset <=  hole->length)           &&
          (size   <= (hole->length - offset)) &&
          (collection = AddArenaCollection(magic, name, offset, size)))
      {
        // Whatever the region is going to keep is none of the business of the arena
        memset(hole->memory + offset, 0, size);
        PublishArenaCollection(magic, collection, WORM_COLLECTION_REGION);
      }
    }

    ClearArenaLock(arena, WORM_LOCK_WRITE, generation);
  }

  return collection;
}

struct WormArenaCollection* CreateWormArenaQueue(struct WormArena* arena, const char* name, uint32_t slots, int timeout)
{
  uint32_t generation;
  uint64_t offset;
  uint64_t length;
  struct WormHole* hole;
  struct WormArenaMagic* magic;
  struct WormArenaCollection* collection;

  if ((arena == NULL) ||
      (name  == NULL) ||
      (slots == 0)    ||
      (slots & (slots - 1)))
  {
    // The count of the slots has to be a power of two, the position is masked by it
    return NULL;
  }

  if (collection = FindWormArenaCollection(arena, name, WORM_COLLECTION_QUEUE))
  {
    // Somebody has created the queue already, it is shared as it is
    return collection;
  }

  hole       = arena->hole;
  magic      = (struct WormArenaMagic*)hole->memory;
  collection = NULL;

  if (SetArenaLock(arena, WORM_LOCK_WRITE, timeout, &generation) == 0)
  {
    if ((collection = FindWormArenaCollection(arena, name, WORM_COLLECTION_QUEUE)) == NULL)
    {
      offset = GetFreeOffset(magic);
      length = ALIGN_OFFSET((uint64_t)slots * sizeof(uint64_t));

      if ((offset <=  hole->length)           &&
          (length <= (hole->length - offset)) &&
          (collection = AddArenaCollection(magic, name, offset, length)))
      {
        // A zeroed table is a table of empty slots, nothing else has to be laid out
        memset(hole->memory + offset, 0, length);

        collection->count = slots;

        atomic_store_explicit(&collection->head, 0, memory_order_relaxed);
        atomic_store_explicit(&collection->tail, 0, memory_order_relaxed);

        PublishArenaCollection(magic, collection, WORM_COLLECTION_QUEUE);
      }
    }

    ClearArenaLock(arena, WORM_LOCK_WRITE, generation);
  }

  return collection;
}

struct WormArenaCollection* CreateWormArenaPool(struct WormArena* arena, const char* name, uint32_t blocks, uint64_t size, int timeout)
{
  uint32_t number;
  uint32_t generation;
  uint64_t offset;
  uint64_t length;
  uint64_t data;
  struct WormHole* hole;
  struct WormArenaMagic* magic;
  struct WormArenaBlock* table;
  struct WormArenaCollection* collection;

  if ((arena  == NULL) ||
      (name   == NULL) ||
      (blocks == 0)    ||
      (size   == 0)    ||
      (size   >  arena->hole->length))
  {
    // A size that big has nowhere to fit anyway, and rounding it up would wrap it to zero
    return NULL;
  }

  if (collection = FindWormArenaCollection(arena, name, WORM_COLLECTION_POOL))
  {
    // Somebody has created the pool already, it is shared as it is
    return collection;
  }

  hole       = arena->hole;
  magic      = (struct WormArenaMagic*)hole->memory;
  collection = NULL;

  if (SetArenaLock(arena, WORM_LOCK_WRITE, timeout, &generation) == 0)
  {
    if ((collection = FindWormArenaCollection(arena, name, WORM_COLLECTION_POOL)) == NULL)
    {
      // The blocks are cut out once and for all: the table comes first, the data
      // of every block follows it aligned, so no two of them share a cache line

      offset = GetFreeOffset(magic);
      length = ALIGN_OFFSET(size);
      data   = ALIGN_OFFSET(offset + (uint64_t)blocks * sizeof(struct WormArenaBlock));
      if ((blocks <   WORM_BLOCK_NONE) &&
          (data   <=  hole->length)    &&
          (length !=  0)               &&
          (blocks <= ((hole->length - data) / length)))
      {
        table = (struct WormArenaBlock*)(hole->memory + offset);

        memset(hole->memory + offset, 0, data + (uint64_t)blocks * length - offset);

        for (number = 0; number < blocks; number ++)
        {
          table[number].offset = data + (uint64_t)number * length;
          atomic_store_explicit(&table[number].next, WORM_BLOCK(0, number + 1), memory_order_relaxed);
        }

        atomic_store_explicit(&table[blocks - 1].next, WORM_BLOCK(0, WORM_BLOCK_NONE), memory_order_relaxed);

        if (collection = AddArenaCollection(magic, name, offset, data + (uint64_t)blocks * length - offset))
        {
          collection->count  = blocks;
          collection->length = length;

          atomic_store_explicit(&collection->head, WORM_BLOCK(0, 0), memory_order_relaxed);

          PublishArenaCollection(magic, collection, WORM_COLLECTION_POOL);
        }
      }
    }

    ClearArenaLock(arena, WORM_LOCK_WRITE, generation);
  }

  return collection;
}

static int IsBlockQueued(struct WormArena* arena, uint32_t collection, uint32_t block)
{
  uint32_t number;
  uint64_t count;
  uint64_t wanted;
  struct WormArenaMagic* magic;
  ATOMIC(uint64_t)* slots;

  magic  = (struct WormArenaMagic*)arena->hole->memory;
  wanted = WORM_SLOT(collection, block);

  for (number = 0; number < WORM_ARENA_COLLECTIONS; number ++)
  {
    if (atomic_load_explicit(&magic->collections[number].type, memory_order_acquire) != WORM_COLLECTION_QUEUE)
    {
      //
      continue;
    }

    slots = (ATOMIC(uint64_t)*)(arena->hole->memory + magic->collections[number].offset);
    count = magic->collections[number].count;

    while (count > 0)
    {
      if (atomic_load_explicit(slots, memory_order_acquire) == wanted)
      {
        // Somebody is still waiting to take this one out
        return 1;
      }

      slots ++;
      count --;
    }
  }

  return 0;
}

static void SweepSpinLock(struct WormArena* arena, struct WormArenaLock* lock)
{
  uint64_t value;

  value = atomic_load_explicit(&lock->value, memory_order_relaxed);

  // Only a writer leaves its number in the word, and only a number that belongs to
  // nobody any more is a reason to cut the lock loose. A lock held by readers has no
  // owner to ask about and is left to its deadline.
  //
  // This is what keeps the number from meaning two different peers at once: the sweep
  // runs before a slot is handed out again, so whatever the peer that had it was
  // holding is released while the number still stands for that very peer

  if ((WORM_LOCK_STATE(value) != 0) &&
      (WORM_LOCK_OWNER(value) != 0) &&
      (IsEntityAlive(arena, WORM_LOCK_OWNER(value)) == 0))
  {
    atomic_compare_exchange_strong_explicit(&lock->value, &value,
      WORM_LOCK(WORM_LOCK_GENERATION(value) + 1, 0, 0, 0), memory_order_release, memory_order_relaxed);
  }
}

static void SweepArena(struct WormArena* arena)
{
  uint32_t type;
  uint32_t number;
  uint32_t index;
  uint64_t count;
  uint64_t alive;
  uint64_t value;
  uint64_t holders;
  struct WormHole* hole;
  struct WormArenaMagic* magic;
  struct WormArenaBlock* block;
  struct WormArenaCollection* collection;
  ATOMIC(uint64_t)* slots;

  hole  = arena->hole;
  magic = (struct WormArenaMagic*)hole->memory;
  alive = WORM_HOLDER_QUEUE;

  // A queue is not an entity and has no lease, so its bit counts as alive: a block
  // waiting in a slot is held by that slot and not by anybody who is gone

  for (index = 1; index < WORM_ARENA_ENTITIES; index ++)
  {
    alive |= (uint64_t)(IsEntityAlive(arena, index) != 0) << index;
  }

  pthread_rwlock_wrlock(&arena->lock);

  SweepSpinLock(arena, &magic->lock);

  for (number = 0; number < WORM_ARENA_COLLECTIONS; number ++)
  {
    collection = magic->collections + number;

    SweepSpinLock(arena, &collection->lock);

    type = atomic_load_explicit(&collection->type, memory_order_acquire);

    if (type == WORM_COLLECTION_QUEUE)
    {
      // A slot a producer has taken and not filled in carries its number, and the
      // number alone stops meaning that very peer once the slot of the entity is
      // handed out again. So the reservation loses its owner here, while the number
      // still stands for the peer that is gone, and the consumer that gets there
      // next takes the slot back the way it does for any producer that is no more

      slots = (ATOMIC(uint64_t)*)(hole->memory + collection->offset);
      count = collection->count;

      while (count > 0)
      {
        value = atomic_load_explicit(slots, memory_order_relaxed);

        if ((WORM_SLOT_RESERVED(value))    &&
            (WORM_SLOT_ENTITY(value) != 0) &&
            (IsEntityAlive(arena, WORM_SLOT_ENTITY(value)) == 0))
        {
          // The position stays as it is, so the token of the producer stops matching
          // whatever it tries to publish once it wakes up
          atomic_compare_exchange_strong_explicit(slots, &value, WORM_SLOT_DISOWNED(value), memory_order_release, memory_order_relaxed);
        }

        slots ++;
        count --;
      }
    }

    if (type != WORM_COLLECTION_POOL)
    {
      //
      continue;
    }

    block = (struct WormArenaBlock*)(hole->memory + collection->offset);
    count = collection->count;

    while (count > 0)
    {
      SweepSpinLock(arena, &block->lock);

      holders = atomic_load_explicit(&block->holders, memory_order_relaxed);
      value   = holders & alive;

      if (holders != value)
      {
        // Whoever is gone takes its count with it, there is nobody left to bring it down

        for (index = 1; index < WORM_ARENA_ENTITIES; index ++)
        {
          if (~alive & holders & WORM_HOLDER(index))
          {
            //
            atomic_store_explicit(&block->counts[index], 0, memory_order_relaxed);
          }
        }

        if ((atomic_compare_exchange_strong_explicit(&block->holders, &holders, value, memory_order_release, memory_order_relaxed)) &&
            (value == 0))
        {
          // Nothing holds the block any more, so it goes back to the free list of its pool

          if (block->name[0] != '\0')
          {
            atomic_fetch_add_explicit(&collection->revision, 1, memory_order_release);
            atomic_fetch_add_explicit(&magic->revisions[WORM_REVISION_COLLECTION], 1, memory_order_release);
          }

          memset(block->name, 0, sizeof(block->name));

          block->size = 0;

          PushFreeBlock(hole, collection, block);
        }
      }

      block ++;
      count --;
    }

    // Whoever pushes a block into a queue takes the hold of the queue before writing
    // the slot, and holds the block itself all along, so a block held by a queue alone
    // and named by no slot is one a producer or a consumer has died in the middle of

    block = (struct WormArenaBlock*)(hole->memory + collection->offset);
    count = collection->count;
    index = 0;

    while (count > 0)
    {
      holders = WORM_HOLDER_QUEUE;

      if ((atomic_load_explicit(&block->holders, memory_order_relaxed) == WORM_HOLDER_QUEUE) &&
          (IsBlockQueued(arena, number, index) == 0)                                         &&
          (atomic_compare_exchange_strong_explicit(&block->holders, &holders, 0, memory_order_release, memory_order_relaxed)))
      {
        atomic_store_explicit(&block->counts[0], 0, memory_order_relaxed);

        if (block->name[0] != '\0')
        {
          atomic_fetch_add_explicit(&collection->revision, 1, memory_order_release);
          atomic_fetch_add_explicit(&magic->revisions[WORM_REVISION_COLLECTION], 1, memory_order_release);
        }

        memset(block->name, 0, sizeof(block->name));

        block->size = 0;

        PushFreeBlock(hole, collection, block);
      }

      block ++;
      count --;
      index ++;
    }
  }

  pthread_rwlock_unlock(&arena->lock);
}

static struct WormArenaBlock* AllocateBlock(struct WormArena* arena, struct WormArenaCollection* pool, const char* name, uint64_t size, int timeout)
{
  uint32_t generation;
  struct WormHole* hole;
  struct WormArenaMagic* magic;
  struct WormArenaBlock* block;
  struct WormArenaBlock* result;

  if ((arena         == NULL)                 ||
      (arena->entity == NULL)                 ||
      (pool          == NULL)                 ||
      (pool->type    != WORM_COLLECTION_POOL) ||
      (size          == 0)                    ||
      (size          >  pool->length))
  {
    // Only a registered entity can own a block, and it has to fit the pool
    return NULL;
  }

  hole   = arena->hole;
  magic  = (struct WormArenaMagic*)hole->memory;
  result = NULL;

  if (SetArenaLock(arena, WORM_LOCK_WRITE, timeout, &generation) == 0)
  {
    block = PopFreeBlock(hole, pool);

    if (block == NULL)
    {
      // Out of blocks is the only moment when giving back what the peers that are
      // gone were holding is worth its price, and the only one that has to pay it
      // is the one that has run out

      SweepArena(arena);

      block = PopFreeBlock(hole, pool);
    }

    if (block != NULL)
    {
      memset(block->name, 0, sizeof(block->name));

      if (name != NULL)
      {
        // A block carrying a message stays nameless
        strncpy(block->name, name, sizeof(block->name) - 1);
      }

      block->size = size;

      if (name != NULL)
      {
        // Only a named block is ever looked for by name, so only it moves a revision.
        // A block taken for a message is nameless and lives as long as the message
        // does, and its coming and going has to cost the neighbours nothing at all

        atomic_fetch_add_explicit(&pool->revision, 1, memory_order_release);
        atomic_fetch_add_explicit(&magic->revisions[WORM_REVISION_COLLECTION], 1, memory_order_release);
      }

      ResetSpinLock(&block->lock);
      atomic_fetch_add_explicit(&block->revision, 1, memory_order_relaxed);
      atomic_store_explicit(&block->counts[arena->entity->number], 1, memory_order_relaxed);

      // The holder is published last, it is what makes the block taken for everybody else
      atomic_store_explicit(&block->holders, WORM_HOLDER(arena->entity->number), memory_order_release);

      result = block;
    }

    ClearArenaLock(arena, WORM_LOCK_WRITE, generation);
  }

  return result;
}

static struct WormArenaBlock* HoldBlock(struct WormArena* arena, struct WormArenaBlock* block)
{
  uint32_t number;
  uint64_t value;
  struct WormArenaBlock* result;

  if ((arena         == NULL) ||
      (arena->entity == NULL) ||
      (block         == NULL))
  {
    //
    return NULL;
  }

  number = arena->entity->number;
  result = NULL;

  // The map and the count are two words, so within a process they are moved under
  // the lock of the arena: otherwise the last release of one thread could free the
  // block between the exchange below and the count that follows it

  pthread_rwlock_wrlock(&arena->lock);

  value = atomic_load_explicit(&block->holders, memory_order_relaxed);

  while (value != 0)
  {
    if (atomic_compare_exchange_weak_explicit(&block->holders, &value, value | WORM_HOLDER(number), memory_order_acquire, memory_order_relaxed))
    {
      if (atomic_fetch_add_explicit(&block->counts[number], 1, memory_order_relaxed) != WORM_HOLDER_LIMIT)
      {
        //
        result = block;
        break;
      }

      // One entity can not hold one block more times than the count can carry
      atomic_fetch_sub_explicit(&block->counts[number], 1, memory_order_relaxed);
      break;
    }
  }

  pthread_rwlock_unlock(&arena->lock);

  return result;
}

static void ReleaseBlock(struct WormArena* arena, struct WormArenaBlock* block)
{
  uint32_t number;
  uint64_t value;
  struct WormArenaCollection* pool;

  if ((arena         == NULL) ||
      (arena->entity == NULL) ||
      (block         == NULL))
  {
    //
    return;
  }

  number = arena->entity->number;

  pthread_rwlock_wrlock(&arena->lock);

  value = 0;

  if (atomic_fetch_sub_explicit(&block->counts[number], 1, memory_order_release) == 1)
  {
    // That was the last hold of ours, so our bit goes out of the map
    value = atomic_fetch_and_explicit(&block->holders, ~WORM_HOLDER(number), memory_order_release);
  }

  pthread_rwlock_unlock(&arena->lock);

  if (value != WORM_HOLDER(number))
  {
    // We hold the block yet, or somebody else does
    return;
  }

  // No lock is taken here on purpose. The free list is a stack of exchanges of its
  // own, and the block belongs to nobody by now, so its name and size are ours to
  // clear. Waiting for the lock of the arena would mean losing the block on a timeout

  if (pool = GetBlockPool(arena, block))
  {
    if (block->name[0] != '\0')
    {
      //
      atomic_fetch_add_explicit(&pool->revision, 1, memory_order_release);
      atomic_fetch_add_explicit(&((struct WormArenaMagic*)arena->hole->memory)->revisions[WORM_REVISION_COLLECTION], 1, memory_order_release);
    }

    memset(block->name, 0, sizeof(block->name));

    block->size = 0;

    PushFreeBlock(arena->hole, pool, block);
  }
}

static int GetBlockNumber(struct WormArena* arena, struct WormArenaBlock* block, uint32_t* collection, uint32_t* number)
{
  uint32_t index;
  struct WormArenaMagic* magic;
  struct WormArenaCollection* pool;

  if ((arena == NULL) ||
      (block == NULL) ||
      ((pool = GetBlockPool(arena, block)) == NULL))
  {
    // The block does not belong to any of the pools of this arena
    return -EINVAL;
  }

  magic       = (struct WormArenaMagic*)arena->hole->memory;
  index       = pool - magic->collections;
  *collection = index;
  *number     = block - (struct WormArenaBlock*)(arena->hole->memory + pool->offset);

  return 0;
}

static struct WormArenaBlock* GetBlock(struct WormArena* arena, uint32_t collection, uint32_t number)
{
  struct WormArenaMagic* magic;
  struct WormArenaCollection* pool;

  if ((arena      == NULL) ||
      (collection == 0)    ||
      (collection >= WORM_ARENA_COLLECTIONS))
  {
    //
    return NULL;
  }

  magic = (struct WormArenaMagic*)arena->hole->memory;
  pool  = magic->collections + collection;

  if ((atomic_load_explicit(&pool->type, memory_order_acquire) != WORM_COLLECTION_POOL) ||
      (number >= pool->count))
  {
    // The numbers come from the shared memory, so they are taken on trust nowhere
    return NULL;
  }

  return (struct WormArenaBlock*)(arena->hole->memory + pool->offset) + number;
}

int QuiesceWormArena(struct WormArena* arena, int timeout)
{
  int result;
  uint32_t number;
  uint32_t generation;
  uint64_t time;
  uint64_t value;
  uint64_t round;
  struct WormArenaMagic* magic;
  struct WormArenaEntity* entity;

  if ((arena         == NULL) ||
      (arena->entity == NULL))
  {
    // Arena or entity are not registered
    return -EINVAL;
  }

  magic  = (struct WormArenaMagic*)arena->hole->memory;
  result = SetArenaLock(arena, WORM_LOCK_READ, timeout, &generation);

  if (result == 0)
  {
    result = -EPIPE;
    value  = WORM_PEER(arena->host, arena->process);
    time   = GetCurrentTime();

    // The slot is only ours while the lease is: one that has run out may have been
    // handed to somebody else by now, and marking it would tell everybody to leave a
    // peer alone that is nothing of the kind

    if ((atomic_load_explicit(&arena->entity->peer, memory_order_relaxed) == value) &&
        (atomic_load_explicit(&arena->entity->time, memory_order_relaxed) >= time))
    {
      // The state goes up before anything is looked at, so that a round seen afterwards
      // is a round of a peer that had the chance to see it

      atomic_fetch_or_explicit(&arena->entity->state, WORM_ENTITY_QUIESCING, memory_order_release);

      for (number = 0; number < WORM_ARENA_ENTITIES; number ++)
      {
        entity = magic->entities + number;
        round  = WORM_ROUND_NONE;

        if ((entity != arena->entity) &&
            (atomic_load_explicit(&entity->time, memory_order_acquire) >= time))
        {
          // Somebody who is here now may be in the middle of a pass that read the state
          // of this peer before it changed, and that pass has to be waited out

          round = WORM_ENTITY_ROUND(atomic_load_explicit(&entity->state, memory_order_acquire));
        }

        arena->rounds[number] = round;
      }

      result = 0;
    }

    ClearArenaLock(arena, WORM_LOCK_READ, generation);
  }

  return result;
}

int CheckWormArenaQuiescence(struct WormArena* arena, int timeout)
{
  int result;
  uint32_t number;
  uint32_t generation;
  uint64_t time;
  uint64_t value;
  struct WormArenaMagic* magic;
  struct WormArenaEntity* entity;

  if ((arena         == NULL) ||
      (arena->entity == NULL))
  {
    // Arena or entity are not registered
    return -EINVAL;
  }

  magic  = (struct WormArenaMagic*)arena->hole->memory;
  result = SetArenaLock(arena, WORM_LOCK_READ, timeout, &generation);

  if (result != 0)
  {
    //
    return result;
  }

  time = GetCurrentTime();

  // The lock is what makes these two a pair rather than two readings that may fall on
  // either side of a slot changing hands: taking one is what registration does. A lease
  // that has run out takes the slot with it, and what stands there now is somebody
  // else's business rather than an answer about this peer

  result = -EPIPE;

  if ((atomic_load_explicit(&arena->entity->peer, memory_order_relaxed) == WORM_PEER(arena->host, arena->process)) &&
      (atomic_load_explicit(&arena->entity->time, memory_order_relaxed) >= time))
  {
    result = -EINVAL;

    if (WORM_ENTITY_STATE(atomic_load_explicit(&arena->entity->state, memory_order_relaxed)) & WORM_ENTITY_QUIESCING)
    {
      //
      result = 0;
    }
  }

  if (result != 0)
  {
    ClearArenaLock(arena, WORM_LOCK_READ, generation);
    return result;
  }

  for (number = 0; number < WORM_ARENA_ENTITIES; number ++)
  {
    entity = magic->entities + number;

    // A neighbour that has gone since is no longer anybody to wait for: its lease is
    // what bounds this, and a peer that stops touching stops being waited on

    if ((arena->rounds[number] != WORM_ROUND_NONE) &&
        (atomic_load_explicit(&entity->time, memory_order_acquire) >= time))
    {
      value = WORM_ENTITY_ROUND(atomic_load_explicit(&entity->state, memory_order_acquire));

      if (value <= arena->rounds[number])
      {
        // The pass this neighbour was in when the state went up is still going on
        ClearArenaLock(arena, WORM_LOCK_READ, generation);
        return -EAGAIN;
      }
    }
  }

  ClearArenaLock(arena, WORM_LOCK_READ, generation);

  return 0;
}

int UpdateWormArena(struct WormArena* arena, int timeout)
{
  if (arena == NULL)
  {
    //
    return -EINVAL;
  }


  // Rebuilds the maps of entities and of named blocks if anything has moved,
  // this is what a peer calls when the bell tells it to look around

  return TrackCollectionUpdate(arena, timeout);
}

// Entities

int RegisterWormArenaEntity(struct WormArena* arena, const char* name, int valid, int timeout)
{
  int result;
  uint32_t number;
  uint32_t generation;
  uint64_t value;
  uint64_t time;
  struct WormHole* hole;
  struct WormArenaMagic* magic;
  struct WormArenaEntity* entity;

  if ((arena         == NULL) ||
      (arena->entity != NULL))
  {
    // Arena is not created or the entity is registered already
    return -EINVAL;
  }

  hole   = arena->hole;
  magic  = (struct WormArenaMagic*)hole->memory;
  result = SetArenaLock(arena, WORM_LOCK_WRITE, timeout, &generation);

  if (result == 0)
  {
    result = -ENOMEM;
    time   = GetCurrentTime();

    // Whatever the peer that has held this slot before was holding is given back
    // first: the number is about to become ours, and with it every bit that carries
    // it. A block of somebody long gone would otherwise be counted as ours forever

    SweepArena(arena);

    // The slot zero is never handed out, its bit in a block belongs to the queues

    for (number = 1; number < WORM_ARENA_ENTITIES; number ++)
    {
      entity = magic->entities + number;

      if (atomic_load_explicit(&entity->time, memory_order_relaxed) < time)
      {
        entity->number = number;

        // The peer says who this is and never changes, the address says where to ring
        // and is handed out by ivshmem anew on every connection, so it is written
        // again on every renewal of the lease

        atomic_store_explicit(&entity->address, (uint32_t)hole->identifier, memory_order_relaxed);
        atomic_store_explicit(&entity->peer, WORM_PEER(arena->host, arena->process), memory_order_relaxed);

        // The round of a slot goes on where the peer before it left off and never back
        // to nothing. A neighbour that was going away has written down where this slot
        // stood, and a round starting afresh would leave the newcomer owing it every
        // pass its predecessor had ever made -- for a departure it could not possibly
        // have been holding up, having read the word before it drew its first breath.
        // Only the state is cleared, and the round takes the step that says the slot
        // has changed hands

        value = atomic_load_explicit(&entity->state, memory_order_relaxed);

        atomic_store_explicit(&entity->state, WORM_ENTITY(WORM_ENTITY_ROUND(value) + 1, 0), memory_order_relaxed);

        memset(entity->name, 0, sizeof(entity->name));
        strncpy(entity->name, name, sizeof(entity->name) - 1);

        // The lease is published last, it makes the entity visible to the peers
        atomic_store_explicit(&entity->time, time + (uint64_t)valid * 1000000ULL, memory_order_release);
        atomic_fetch_add_explicit(&magic->revisions[WORM_REVISION_ENTITY], 1, memory_order_release);

        arena->entity = entity;
        result        = 0;
        break;
      }
    }

    ClearArenaLock(arena, WORM_LOCK_WRITE, generation);
  }

  return result;
}

int TouchWormArena(struct WormArena* arena, int valid, int timeout)
{
  int result;
  uint32_t generation;
  uint64_t value;
  uint64_t time;
  struct WormHole* hole;
  struct WormArenaEntity* entity;

  if ((arena         == NULL) ||
      (arena->entity == NULL))
  {
    // Arena or entity are not registered
    return -EINVAL;
  }

  hole   = arena->hole;
  result = SetArenaLock(arena, WORM_LOCK_READ, timeout, &generation);

  if (result == 0)
  {
    result = -EPIPE;
    entity = arena->entity;
    value  = WORM_PEER(arena->host, arena->process);
    time   = GetCurrentTime();

    if ((atomic_load_explicit(&entity->peer, memory_order_relaxed) == value) &&
        (atomic_load_explicit(&entity->time, memory_order_relaxed) >= time))
    {
      // The lease is still valid and belongs to us, so it can be extended. The round
      // goes up with it: a touch is what a peer does between one pass of its work and
      // the next, and that is what makes it a mark a departing neighbour can wait on

      atomic_store_explicit(&entity->address, (uint32_t)hole->identifier, memory_order_relaxed);
      atomic_fetch_add_explicit(&entity->state, WORM_ENTITY_ROUND_STEP, memory_order_release);
      atomic_store_explicit(&entity->time, time + (uint64_t)valid * 1000000ULL, memory_order_release);
      result = 0;
    }

    ClearArenaLock(arena, WORM_LOCK_READ, generation);
  }

  return result;
}

// Descriptors

static struct WormArenaDescriptor* MakeArenaDescriptor(struct WormArena* arena, struct WormArenaBlock* block)
{
  struct WormArenaDescriptor* descriptor;

  if (descriptor = (struct WormArenaDescriptor*)calloc(1, sizeof(struct WormArenaDescriptor)))
  {
    descriptor->arena    = arena;
    descriptor->block    = block;
    descriptor->data     = arena->hole->memory + block->offset;
    descriptor->revision = atomic_load_explicit(&block->revision, memory_order_acquire);
  }

  return descriptor;
}

struct WormArenaDescriptor* CreateWormArenaDescriptor(struct WormArena* arena, struct WormArenaCollection* pool, const char* name, uint64_t size, int timeout)
{
  struct WormArenaBlock* block;
  struct WormArenaDescriptor* descriptor;

  if ((block = AllocateBlock(arena, pool, name, size, timeout)) == NULL)
  {
    //
    return NULL;
  }

  if ((descriptor = MakeArenaDescriptor(arena, block)) == NULL)
  {
    // There is nothing to give the block back through
    ReleaseBlock(arena, block);
  }

  return descriptor;
}

struct WormArenaDescriptor* AttachWormArenaDescriptor(struct WormArena* arena, struct WormArenaBlock* block)
{
  struct WormArenaDescriptor* descriptor;

  if (HoldBlock(arena, block) == NULL)
  {
    // The block has been released while we were reaching for it
    return NULL;
  }

  if ((descriptor = MakeArenaDescriptor(arena, block)) == NULL)
  {
    //
    ReleaseBlock(arena, block);
  }

  return descriptor;
}

struct WormArenaDescriptor* OpenWormArenaDescriptor(struct WormArena* arena, const char* name)
{
  void* value;
  int result;
  struct WormArenaDescriptor* descriptor;

  if ((arena == NULL) ||
      (name  == NULL))
  {
    //
    return NULL;
  }

  pthread_rwlock_rdlock(&arena->lock);
  result = GetFromHashMap(arena->blocks, name, strlen(name), &value);
  pthread_rwlock_unlock(&arena->lock);

  descriptor = NULL;

  if (result == HASHMAP_SUCCESS)
  {
    // The map is a snapshot, so the block still has to be held before it is used
    descriptor = AttachWormArenaDescriptor(arena, (struct WormArenaBlock*)value);
  }

  return descriptor;
}

void ReleaseWormArenaDescriptor(struct WormArenaDescriptor* descriptor)
{
  if (descriptor != NULL)
  {
    // The lock goes before the reference: a lock of a block nobody references any
    // more would be left standing until its deadline runs out

    UnlockWormArenaDescriptor(descriptor);
    ReleaseBlock(descriptor->arena, descriptor->block);

    free(descriptor);
  }
}

int LockWormArenaDescriptor(struct WormArenaDescriptor* descriptor, int type, int timeout)
{
  int result;

  if ((descriptor        == NULL) ||
      (descriptor->state != 0))
  {
    // Nothing here is meant to be locked twice
    return -EINVAL;
  }

  result = SetBlockLock(descriptor->arena, descriptor->block, type, timeout, &descriptor->generation);

  if (result == 0)
  {
    descriptor->state    = type;
    descriptor->revision = atomic_load_explicit(&descriptor->block->revision, memory_order_relaxed);
  }

  return result;
}

void UnlockWormArenaDescriptor(struct WormArenaDescriptor* descriptor)
{
  if ((descriptor        != NULL) &&
      (descriptor->state != 0))
  {
    if (descriptor->state == WORM_LOCK_WRITE)
    {
      // Whoever looks at the block next has to see that the data are not the same any more
      descriptor->revision = atomic_fetch_add_explicit(&descriptor->block->revision, 1, memory_order_relaxed) + 1;
    }

    ClearBlockLock(descriptor->block, descriptor->state, descriptor->generation);

    descriptor->state = 0;
  }
}

int CheckWormArenaDescriptor(struct WormArenaDescriptor* descriptor)
{
  uint64_t revision;

  if (descriptor == NULL)
  {
    //
    return -EINVAL;
  }

  revision = atomic_load_explicit(&descriptor->block->revision, memory_order_acquire);

  if (revision == descriptor->revision)
  {
    // Nothing has happened to the block since the last look at it
    return 0;
  }

  descriptor->revision = revision;

  return 1;
}

// Queues

static ATOMIC(uint64_t)* TakePosition(struct WormArena* arena, struct WormArenaCollection* queue, ATOMIC(uint64_t)* counter, int busy, uint64_t* result)
{
  uint64_t mask;
  uint64_t value;
  uint64_t position;
  ATOMIC(uint64_t)* slot;
  ATOMIC(uint64_t)* slots;

  // The value of the slot is the whole state of it: a producer needs it empty,
  // a consumer needs it filled, and the counter is only a hint where to look

  slots    = (ATOMIC(uint64_t)*)(arena->hole->memory + queue->offset);
  mask     = queue->count - 1;
  position = atomic_load_explicit(counter, memory_order_relaxed);

  while (1)
  {
    slot  = slots + (position & mask);
    value = atomic_load_explicit(slot, memory_order_acquire);

    if ((value != WORM_SLOT_EMPTY) == busy)
    {
      if (atomic_compare_exchange_weak_explicit(counter, &position, position + 1, memory_order_relaxed, memory_order_relaxed))
      {
        *result = position;
        return slot;
      }

      // The position has been taken by somebody else and reloaded by the exchange
      continue;
    }

    if (position == atomic_load_explicit(counter, memory_order_relaxed))
    {
      // Full for a producer, empty for a consumer, and the counter is where it was
      return NULL;
    }

    position = atomic_load_explicit(counter, memory_order_relaxed);
  }
}

int PushWormArenaQueue(struct WormArena* arena, struct WormArenaCollection* queue, struct WormArenaDescriptor* descriptor)
{
  int result;
  uint32_t collection;
  uint32_t number;
  uint64_t token;
  uint64_t value;
  uint64_t position;
  ATOMIC(uint64_t)* slot;

  if ((arena         == NULL) ||
      (queue         == NULL) ||
      (descriptor    == NULL) ||
      (arena->entity == NULL) ||
      (atomic_load_explicit(&queue->type, memory_order_acquire) != WORM_COLLECTION_QUEUE))
  {
    //
    return -EINVAL;
  }

  result = GetBlockNumber(arena, descriptor->block, &collection, &number);

  if (result < 0)
  {
    // The block does not belong to any pool of this arena
    return result;
  }

  // The queue becomes a holder before anything is done to the slot: dying here
  // leaves a bit for the sweep to clear, but never a slot nobody can make sense of.
  // Taking it back is safe because the producer still holds the block itself

  if (atomic_fetch_add_explicit(&descriptor->block->counts[0], 1, memory_order_relaxed) == WORM_HOLDER_LIMIT)
  {
    // One block can not wait in more slots than the count carries
    atomic_fetch_sub_explicit(&descriptor->block->counts[0], 1, memory_order_relaxed);
    return -ENOBUFS;
  }

  atomic_fetch_or_explicit(&descriptor->block->holders, WORM_HOLDER_QUEUE, memory_order_relaxed);

  result = -ENOBUFS;
  slot   = TakePosition(arena, queue, &queue->tail, 0, &position);

  if (slot != NULL)
  {
    value  = WORM_SLOT_EMPTY;
    token  = WORM_SLOT_RESERVE(position, arena->entity->number);
    result = -EAGAIN;

    if (atomic_compare_exchange_strong_explicit(slot, &value, token, memory_order_acquire, memory_order_relaxed))
    {
      // From here on the slot carries our entity, so a consumer running into it can
      // ask the lease whether we are still around to fill it in. And the reservation
      // is a token to be given back rather than a place to write to: a peer slow
      // enough to lose its lease finds the slot taken away and its position long
      // behind the head, so publishing by a plain store would put a message where
      // nobody will ever look and hold the block for good

      if (atomic_compare_exchange_strong_explicit(slot, &token, WORM_SLOT(collection, number), memory_order_release, memory_order_relaxed))
      {
        // Nothing is counted here: a slot carries a block that has no name, so it
        // changes no set anybody looks things up in, and a consumer learns of the
        // message from the doorbell and from the slot itself

        return 0;
      }

      // The token is gone, which means we have been taken for dead and everything we
      // were holding has already been given back on our behalf, the hold of the queue
      // and the block itself included. There is nothing here left to undo, and the
      // descriptor of the caller does not stand for anything any more

      return -EOWNERDEAD;
    }
  }

  if (atomic_fetch_sub_explicit(&descriptor->block->counts[0], 1, memory_order_relaxed) == 1)
  {
    //
    atomic_fetch_and_explicit(&descriptor->block->holders, ~WORM_HOLDER_QUEUE, memory_order_relaxed);
  }

  return result;
}

struct WormArenaDescriptor* PopWormArenaQueue(struct WormArena* arena, struct WormArenaCollection* queue)
{
  uint64_t mask;
  uint64_t value;
  uint64_t holder;
  uint64_t position;
  ATOMIC(uint64_t)* slot;
  ATOMIC(uint64_t)* slots;
  struct WormArenaBlock* block;
  struct WormArenaDescriptor* descriptor;

  if ((arena         == NULL) ||
      (queue         == NULL) ||
      (arena->entity == NULL) ||
      (atomic_load_explicit(&queue->type, memory_order_acquire) != WORM_COLLECTION_QUEUE))
  {
    //
    return NULL;
  }

  // The descriptor is taken first on purpose: once the slot has been emptied there is
  // no way back, and a failure here would leave the reference with nobody to hold it

  if ((descriptor = (struct WormArenaDescriptor*)calloc(1, sizeof(struct WormArenaDescriptor))) == NULL)
  {
    //
    return NULL;
  }

  slots    = (ATOMIC(uint64_t)*)(arena->hole->memory + queue->offset);
  mask     = queue->count - 1;
  position = atomic_load_explicit(&queue->head, memory_order_relaxed);

  while (1)
  {
    slot  = slots + (position & mask);
    value = atomic_load_explicit(slot, memory_order_acquire);

    if (value == WORM_SLOT_EMPTY)
    {
      if (position == atomic_load_explicit(&queue->head, memory_order_relaxed))
      {
        // Nothing has been pushed at this position, the queue is empty
        free(descriptor);
        return NULL;
      }

      position = atomic_load_explicit(&queue->head, memory_order_relaxed);
      continue;
    }

    if (WORM_SLOT_RESERVED(value))
    {
      if (IsEntityAlive(arena, WORM_SLOT_ENTITY(value)))
      {
        // The producer is there and is about to fill the slot in, it is a matter
        // of a few instructions and not of ours to wait for
        free(descriptor);
        return NULL;
      }

      // The producer is gone for good, so the slot is dropped and stepped over

      if (atomic_compare_exchange_strong_explicit(slot, &value, WORM_SLOT_EMPTY, memory_order_release, memory_order_relaxed))
      {
        atomic_compare_exchange_strong_explicit(&queue->head, &position, position + 1, memory_order_relaxed, memory_order_relaxed);
      }

      position = atomic_load_explicit(&queue->head, memory_order_relaxed);
      continue;
    }

    if (atomic_compare_exchange_weak_explicit(&queue->head, &position, position + 1, memory_order_relaxed, memory_order_relaxed))
    {
      // The position belongs to us, and only its owner empties the slot
      break;
    }
  }

  if ((block = GetBlock(arena, WORM_SLOT_COLLECTION(value), WORM_SLOT_BLOCK(value))) == NULL)
  {
    // The slot carries the numbers of nothing, it is dropped as it is
    atomic_store_explicit(slot, WORM_SLOT_EMPTY, memory_order_release);
    free(descriptor);
    return NULL;
  }

  // The order here is what makes a crash of a consumer survivable. The hold comes
  // first, so that the block never stands as held by a queue alone while no slot
  // points at it: that state is what the sweep takes for garbage. Dying before the
  // slot is cleared leaves the message in the queue for somebody else to take, and
  // dying after it leaves our hold to the sweep along with the entity of ours

  holder = WORM_HOLDER(arena->entity->number);

  pthread_rwlock_wrlock(&arena->lock);

  atomic_fetch_add_explicit(&block->counts[arena->entity->number], 1, memory_order_relaxed);
  atomic_fetch_or_explicit(&block->holders, holder, memory_order_acquire);

  pthread_rwlock_unlock(&arena->lock);

  atomic_store_explicit(slot, WORM_SLOT_EMPTY, memory_order_release);

  if (atomic_fetch_sub_explicit(&block->counts[0], 1, memory_order_release) == 1)
  {
    //
    atomic_fetch_and_explicit(&block->holders, ~WORM_HOLDER_QUEUE, memory_order_release);
  }

  descriptor->arena    = arena;
  descriptor->block    = block;
  descriptor->data     = arena->hole->memory + block->offset;
  descriptor->revision = atomic_load_explicit(&block->revision, memory_order_acquire);

  return descriptor;
}

// Creation of the arena

static int FormatArena(struct WormHole* hole, const char* name)
{
  struct WormArenaMagic* magic;

  magic = (struct WormArenaMagic*)hole->memory;

  if (hole->length < ALIGN_OFFSET(sizeof(struct WormArenaMagic)))
  {
    // The shared memory is too small to keep even the magic alone
    return -ENOMEM;
  }

  // The region can be left over from a previous incarnation of the arena, so neither
  // the entities, nor the collections, nor the global lock can be trusted to be clean

  memset(magic->name,        0, sizeof(magic->name));
  memset(magic->entities,    0, sizeof(magic->entities));
  memset(magic->collections, 0, sizeof(magic->collections));
  strncpy(magic->name, name, sizeof(magic->name) - 1);

  ResetSpinLock(&magic->lock);
  atomic_store_explicit(&magic->revisions[WORM_REVISION_ENTITY],     0, memory_order_relaxed);
  atomic_store_explicit(&magic->revisions[WORM_REVISION_COLLECTION], 0, memory_order_relaxed);

  return 0;
}

static int CheckArena(struct WormHole* hole, const char* name)
{
  uint32_t number;
  uint32_t type;
  uint64_t length;
  struct WormArenaMagic* magic;
  struct WormArenaCollection* collection;

  magic = (struct WormArenaMagic*)hole->memory;
  if (strncmp(magic->name, name, sizeof(magic->name)) != 0)
  {
    //
    return -EPROTO;
  }

  // The collections have been laid out by another peer, so their bounds are
  // taken on trust unless they are checked against the region right here

  for (number = 0; number < WORM_ARENA_COLLECTIONS; number ++)
  {
    collection = magic->collections + number;
    type       = atomic_load_explicit(&collection->type, memory_order_acquire);

    if (type == 0)
    {
      //
      continue;
    }

    // An entry of a pool is a block, an entry of a queue is a single word, and a
    // region has no entries at all

    length = (type == WORM_COLLECTION_POOL)  * sizeof(struct WormArenaBlock) +
             (type == WORM_COLLECTION_QUEUE) * sizeof(uint64_t);

    if ((collection->offset <  sizeof(struct WormArenaMagic))      ||
        (collection->offset >  hole->length)                       ||
        (collection->size   > (hole->length - collection->offset)) ||
        ((length != 0) && (collection->count > (collection->size / length))))
    {
      //
      return -EPROTO;
    }
  }

  return 0;
}

struct WormArena* CreateWormArena(struct WormHole* hole, const char* name)
{
  int result;
  uint64_t value;
  uint64_t limit;
  struct WormArena* arena;
  struct WormArenaMagic* magic;

  if ((hole         == NULL) ||
      (hole->memory == NULL) ||
      (hole->length  < sizeof(struct WormArenaMagic)))
  {
    // There is nothing to lay the arena out on
    return NULL;
  }

  magic  = (struct WormArenaMagic*)hole->memory;
  limit  = GetCurrentTime() + WORM_ARENA_TIMEOUT * 1000000ULL;
  result = -ETIME;

  do
  {
    value = 0;

    if (atomic_compare_exchange_strong_explicit(&magic->signature, &value, WORM_ARENA_CLAIM, memory_order_acquire, memory_order_relaxed))
    {
      // The region is untouched and the claim belongs to us, so the layout is ours to make.
      // On a failure the claim is dropped back to let another peer try on its own
      result = FormatArena(hole, name);
      atomic_store_explicit(&magic->signature, (result == 0) * WORM_ARENA_SIGNATURE, memory_order_release);
      break;
    }

    if (value == WORM_ARENA_SIGNATURE)
    {
      // The arena has been laid out already, it is enough to be sure it is the one we expect
      result = CheckArena(hole, name);
      break;
    }

    if (value != WORM_ARENA_CLAIM)
    {
      // The region carries neither our signature nor a claim, so it belongs to
      // somebody else or to another version of the layout. Waiting will not help
      result = -EPROTO;
      break;
    }

    // Somebody else holds the claim right now
  }
  while (GetCurrentTime() < limit);

  if (result != 0)
  {
    //
    return NULL;
  }

  if (arena = (struct WormArena*)calloc(1, sizeof(struct WormArena)))
  {
    arena->hole     = hole;
    arena->host     = GetHostIdentifier();
    arena->process  = getpid();
    arena->blocks   = CreateHashMap(NULL);

    pthread_rwlock_init(&arena->lock, NULL);

    if ((arena->blocks == NULL) ||
        (TrackCollectionUpdate(arena, WORM_ARENA_TIMEOUT) != 0))
    {
      ReleaseWormArena(arena);
      return NULL;
    }
  }

  return arena;
}

void ReleaseWormArena(struct WormArena* arena)
{
  uint32_t generation;
  struct WormArenaMagic* magic;
  struct WormArenaEntity* entity;

  if (arena != NULL)
  {
    if (entity = arena->entity)
    {
      magic = (struct WormArenaMagic*)arena->hole->memory;

      if (SetArenaLock(arena, WORM_LOCK_WRITE, WORM_ARENA_TIMEOUT, &generation) == 0)
      {
        // Only if the slot is still ours. A lease that ran out while this peer was not
        // looking may have been handed to somebody else, and letting go of it then would
        // put out a peer that is very much alive. The two are read under the lock that
        // registration takes, so they are a pair and not two readings that may fall on
        // either side of the slot changing hands

        if ((atomic_load_explicit(&entity->peer, memory_order_relaxed) == WORM_PEER(arena->host, arena->process)) &&
            (atomic_load_explicit(&entity->time, memory_order_relaxed) >= GetCurrentTime()))
        {
          // Drop the lease right away instead of leaving the slot to the peers to expire
          atomic_store_explicit(&entity->peer, 0, memory_order_relaxed);
          atomic_store_explicit(&entity->time, 0, memory_order_relaxed);
          atomic_fetch_add_explicit(&magic->revisions[WORM_REVISION_ENTITY], 1, memory_order_release);
        }

        ClearArenaLock(arena, WORM_LOCK_WRITE, generation);
      }
    }

    ReleaseHashMap(arena->blocks);
    pthread_rwlock_destroy(&arena->lock);
    free(arena);
  }
}
