#include "WormArena.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

// The arena needs nothing of a WormHole but the shared memory itself, so a plain
// anonymous mapping stands in for ivshmem and fork() stands in for the peers

#define ARENA_LENGTH  (4 * 1024 * 1024)
#define ARENA_TIMEOUT  1000
#define ARENA_LEASE   30000

#define POOL_BLOCKS  64
#define POOL_LENGTH  256
#define QUEUE_SLOTS  16
#define PEERS         4

static int failures = 0;

static void Check(int condition, const char* format, ...)
{
  va_list arguments;

  va_start(arguments, format);
  fputs(condition ? "  ok   " : "  FAIL ", stdout);
  vprintf(format, arguments);
  fputc('\n', stdout);
  va_end(arguments);

  failures += !condition;
}

static struct WormHole* MakeHole(uint8_t* memory, size_t length, uint64_t identifier)
{
  struct WormHole* hole;

  hole             = (struct WormHole*)calloc(1, sizeof(struct WormHole));
  hole->type       = HOLE_CLIENT_TYPE_GUEST;
  hole->memory     = memory;
  hole->length     = length;
  hole->identifier = identifier;
  hole->handle     = -1;

  return hole;
}

// Everything a single peer does on its own

static void TestArena(uint8_t* memory)
{
  int number;
  int count;
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* pool;
  struct WormArenaCollection* queue;
  struct WormArenaCollection* region;
  struct WormArenaCollection* single;
  struct WormArenaDescriptor* descriptor;
  struct WormArenaDescriptor* blocks[POOL_BLOCKS];

  printf("arena\n");

  hole  = MakeHole(memory, ARENA_LENGTH, 1);
  arena = CreateWormArena(hole, "test");

  Check(arena != NULL, "arena is created");
  Check(CreateWormArena(hole, "other") == NULL, "arena of another name is refused");
  Check(RegisterWormArenaEntity(arena, "first", ARENA_LEASE, ARENA_TIMEOUT) == 0, "entity is registered");
  Check(RegisterWormArenaEntity(arena, "first", ARENA_LEASE, ARENA_TIMEOUT) < 0, "entity is not registered twice");
  Check(TouchWormArena(arena, ARENA_LEASE, ARENA_TIMEOUT) == 0, "lease is extended");

  pool   = CreateWormArenaPool(arena, "messages", POOL_BLOCKS, POOL_LENGTH, ARENA_TIMEOUT);
  region = CreateWormArenaRegion(arena, "state", 4096, ARENA_TIMEOUT);
  queue  = CreateWormArenaQueue(arena, "events", QUEUE_SLOTS, ARENA_TIMEOUT);

  Check(pool   != NULL, "pool is created");
  Check(region != NULL, "region is created");
  Check(queue  != NULL, "queue is created");
  Check(CreateWormArenaPool(arena, "messages", POOL_BLOCKS, POOL_LENGTH, ARENA_TIMEOUT) == pool, "pool of the same name is shared");
  Check(CreateWormArenaQueue(arena, "events", 32, ARENA_TIMEOUT) == queue, "queue of the same name is shared");
  Check(CreateWormArenaQueue(arena, "odd", 15, ARENA_TIMEOUT) == NULL, "queue of an odd size is refused");
  Check(FindWormArenaCollection(arena, "messages", WORM_COLLECTION_QUEUE) == NULL, "a pool is not found as a queue");

  printf("blocks\n");

  descriptor = CreateWormArenaDescriptor(arena, pool, "object", 100, ARENA_TIMEOUT);

  Check(descriptor != NULL, "block is allocated");
  Check(CreateWormArenaDescriptor(arena, pool, "object", POOL_LENGTH + 1, ARENA_TIMEOUT) == NULL, "block larger than the pool is refused");

  Check(LockWormArenaDescriptor(descriptor, WORM_LOCK_WRITE, ARENA_TIMEOUT) == 0, "block is locked");
  Check(LockWormArenaDescriptor(descriptor, WORM_LOCK_WRITE, ARENA_TIMEOUT) < 0, "block is not locked twice");

  strcpy((char*)descriptor->data, "hello");
  UnlockWormArenaDescriptor(descriptor);

  Check(CheckWormArenaDescriptor(descriptor) == 0, "the writer sees no change of its own");
  Check(UpdateWormArena(arena, ARENA_TIMEOUT) == 0, "maps are updated");

  blocks[0] = OpenWormArenaDescriptor(arena, "object");

  Check(blocks[0] != NULL, "block is found by name");
  Check((blocks[0] != NULL) && (strcmp((char*)blocks[0]->data, "hello") == 0), "block keeps the data");

  ReleaseWormArenaDescriptor(blocks[0]);

  Check(LockWormArenaDescriptor(descriptor, WORM_LOCK_READ, ARENA_TIMEOUT) == 0, "the second descriptor outlives the first one released");
  UnlockWormArenaDescriptor(descriptor);

  ReleaseWormArenaDescriptor(descriptor);

  printf("pool\n");

  count = 0;

  while ((count < POOL_BLOCKS) &&
         (blocks[count] = CreateWormArenaDescriptor(arena, pool, NULL, POOL_LENGTH, ARENA_TIMEOUT)))
  {
    count ++;
  }

  Check(count == POOL_BLOCKS, "pool runs out after its capacity, taken %d of %d", count, POOL_BLOCKS);

  for (number = 0; number < count; number ++)
  {
    ReleaseWormArenaDescriptor(blocks[number]);
  }

  blocks[0] = CreateWormArenaDescriptor(arena, pool, NULL, POOL_LENGTH, ARENA_TIMEOUT);

  Check(blocks[0] != NULL, "released blocks come back to the pool");

  ReleaseWormArenaDescriptor(blocks[0]);

  printf("queue\n");

  Check(PopWormArenaQueue(arena, queue) == NULL, "empty queue gives nothing");

  descriptor = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);
  strcpy((char*)descriptor->data, "message");

  Check(PushWormArenaQueue(arena, queue, descriptor) == 0, "message is pushed");

  ReleaseWormArenaDescriptor(descriptor);

  descriptor = PopWormArenaQueue(arena, queue);

  Check(descriptor != NULL, "message is popped");
  Check((descriptor != NULL) && (strcmp((char*)descriptor->data, "message") == 0), "message survives the queue");

  ReleaseWormArenaDescriptor(descriptor);

  count = 0;

  while (count < QUEUE_SLOTS + 1)
  {
    descriptor = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);

    if (PushWormArenaQueue(arena, queue, descriptor) < 0)
    {
      ReleaseWormArenaDescriptor(descriptor);
      break;
    }

    ReleaseWormArenaDescriptor(descriptor);
    count ++;
  }

  Check(count == QUEUE_SLOTS, "queue takes exactly its capacity, taken %d of %d", count, QUEUE_SLOTS);

  while (descriptor = PopWormArenaQueue(arena, queue))
  {
    ReleaseWormArenaDescriptor(descriptor);
    count --;
  }

  Check(count == 0, "everything pushed comes back out, left %d", count);

  printf("garbage\n");

  // A message waiting in a slot is held by the queue and has to survive the sweep

  descriptor = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);
  strcpy((char*)descriptor->data, "kept");

  PushWormArenaQueue(arena, queue, descriptor);
  ReleaseWormArenaDescriptor(descriptor);

  Check(UpdateWormArena(arena, ARENA_TIMEOUT) == 0, "the arena is swept with a message in the queue");

  descriptor = PopWormArenaQueue(arena, queue);

  Check(descriptor != NULL, "the message in the queue survives the sweep");
  Check((descriptor != NULL) && (strcmp((char*)descriptor->data, "kept") == 0), "and keeps its data");

  ReleaseWormArenaDescriptor(descriptor);

  // A hold of a queue that no slot stands behind is exactly what a peer dying in the
  // middle of a push or of a pop leaves behind, so it is made here by hand

  single     = CreateWormArenaPool(arena, "single", 1, POOL_LENGTH, ARENA_TIMEOUT);
  descriptor = CreateWormArenaDescriptor(arena, single, NULL, 8, ARENA_TIMEOUT);

  atomic_fetch_add_explicit(&descriptor->block->counts[0], 1, memory_order_relaxed);
  atomic_fetch_or_explicit(&descriptor->block->holders, WORM_HOLDER_QUEUE, memory_order_relaxed);

  ReleaseWormArenaDescriptor(descriptor);

  Check(CreateWormArenaDescriptor(arena, single, NULL, 8, ARENA_TIMEOUT) == NULL, "the block is held by a queue that is not there");
  Check(UpdateWormArena(arena, ARENA_TIMEOUT) == 0, "the arena is swept again");

  descriptor = CreateWormArenaDescriptor(arena, single, NULL, 8, ARENA_TIMEOUT);

  Check(descriptor != NULL, "the hold nobody stands behind is collected");

  ReleaseWormArenaDescriptor(descriptor);
  ReleaseWormArena(arena);
  free(hole);
}

// The same arena seen by several processes at once

static void RunPeer(uint8_t* memory, int number)
{
  char name[32];
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* pool;
  struct WormArenaCollection* queue;
  struct WormArenaDescriptor* descriptor;

  snprintf(name, sizeof(name), "peer%d", number);

  hole  = MakeHole(memory, ARENA_LENGTH, number + 1);
  arena = CreateWormArena(hole, "shared");

  if (arena == NULL)
  {
    //
    _exit(1);
  }

  if (RegisterWormArenaEntity(arena, name, ARENA_LEASE, ARENA_TIMEOUT) != 0)
  {
    //
    _exit(5);
  }

  pool  = CreateWormArenaPool(arena, "messages", POOL_BLOCKS, POOL_LENGTH, ARENA_TIMEOUT);
  queue = CreateWormArenaQueue(arena, "events", QUEUE_SLOTS, ARENA_TIMEOUT);

  if ((pool  == NULL) ||
      (queue == NULL))
  {
    _exit(2);
  }

  if ((descriptor = CreateWormArenaDescriptor(arena, pool, NULL, POOL_LENGTH, ARENA_TIMEOUT)) == NULL)
  {
    _exit(3);
  }

  snprintf((char*)descriptor->data, POOL_LENGTH, "%s", name);

  if (PushWormArenaQueue(arena, queue, descriptor) != 0)
  {
    _exit(4);
  }

  ReleaseWormArenaDescriptor(descriptor);
  ReleaseWormArena(arena);

  _exit(0);
}

static void TestPeers(uint8_t* memory)
{
  int number;
  int status;
  int count;
  int broken;
  pid_t children[PEERS];
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* queue;
  struct WormArenaDescriptor* descriptor;

  printf("peers\n");

  for (number = 0; number < PEERS; number ++)
  {
    // Every peer lays the arena out on its own, exactly one of them wins the claim
    children[number] = fork();

    if (children[number] == 0)
    {
      //
      RunPeer(memory, number);
    }
  }

  broken = 0;

  for (number = 0; number < PEERS; number ++)
  {
    waitpid(children[number], &status, 0);

    if ((!WIFEXITED(status)) || (WEXITSTATUS(status) != 0))
    {
      printf("       peer %d exited with %d\n", number, WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status));
      broken ++;
    }
  }

  Check(broken == 0, "all the peers have got through, broken %d of %d", broken, PEERS);

  hole  = MakeHole(memory, ARENA_LENGTH, PEERS + 1);
  arena = CreateWormArena(hole, "shared");

  Check(arena != NULL, "the arena laid out by a peer is attached to");

  if (arena != NULL)
  {
    RegisterWormArenaEntity(arena, "reader", ARENA_LEASE, ARENA_TIMEOUT);

    queue = FindWormArenaCollection(arena, "events", WORM_COLLECTION_QUEUE);
    count = 0;

    Check(queue != NULL, "the queue of the peers is found");

    while (queue && (descriptor = PopWormArenaQueue(arena, queue)))
    {
      count ++;
      ReleaseWormArenaDescriptor(descriptor);
    }

    Check(count == PEERS, "every peer has left a message, got %d of %d", count, PEERS);

    ReleaseWormArena(arena);
  }

  free(hole);
}

// A peer that is gone without giving anything back

static void RunOrphan(uint8_t* memory)
{
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* pool;

  hole  = MakeHole(memory, ARENA_LENGTH, 9);
  arena = CreateWormArena(hole, "orphan");

  if ((arena == NULL) ||
      (RegisterWormArenaEntity(arena, "orphan", 200, ARENA_TIMEOUT) != 0))
  {
    //
    _exit(1);
  }

  pool = CreateWormArenaPool(arena, "messages", 2, POOL_LENGTH, ARENA_TIMEOUT);

  if ((pool == NULL) ||
      (CreateWormArenaDescriptor(arena, pool, "left", 8, ARENA_TIMEOUT) == NULL))
  {
    //
    _exit(2);
  }

  // No release of anything, exactly as a peer that has been killed
  _exit(0);
}

static void TestRecovery(uint8_t* memory)
{
  int status;
  pid_t child;
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* pool;
  struct WormArenaDescriptor* first;
  struct WormArenaDescriptor* second;

  printf("recovery\n");

  child = fork();

  if (child == 0)
  {
    //
    RunOrphan(memory);
  }

  waitpid(child, &status, 0);

  Check(WIFEXITED(status) && (WEXITSTATUS(status) == 0), "the peer has taken a block and gone");

  hole  = MakeHole(memory, ARENA_LENGTH, 10);
  arena = CreateWormArena(hole, "orphan");

  Check(arena != NULL, "the arena of the peer is attached to");

  if (arena == NULL)
  {
    //
    return;
  }

  RegisterWormArenaEntity(arena, "sweeper", ARENA_LEASE, ARENA_TIMEOUT);

  pool  = FindWormArenaCollection(arena, "messages", WORM_COLLECTION_POOL);
  first = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);

  Check(pool  != NULL, "the pool of the peer is found");
  Check(first != NULL, "the block the peer has not taken is free");
  Check(CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT) == NULL, "the block of the peer is held by the lease of it");

  // The lease of the peer is 200 ms, so by now there is nobody behind that block

  usleep(400000);

  Check(UpdateWormArena(arena, ARENA_TIMEOUT) == 0, "the arena is swept");

  second = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);

  Check(second != NULL, "the block of the dead peer has come back to the pool");

  ReleaseWormArenaDescriptor(second);
  ReleaseWormArenaDescriptor(first);
  ReleaseWormArena(arena);

  free(hole);
}

// The very slot of a peer that is gone, handed out to somebody else

static void TestReuse(uint8_t* memory)
{
  int status;
  pid_t child;
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* pool;
  struct WormArenaDescriptor* first;
  struct WormArenaDescriptor* second;

  printf("reuse\n");

  child = fork();

  if (child == 0)
  {
    //
    RunOrphan(memory);
  }

  waitpid(child, &status, 0);

  // Nobody registers until the lease of the peer has run out, so its very slot is
  // the first free one and the peer coming next gets the number it had

  usleep(400000);

  hole  = MakeHole(memory, ARENA_LENGTH, 11);
  arena = CreateWormArena(hole, "orphan");

  Check(arena != NULL, "the arena of the peer is attached to");

  if (arena == NULL)
  {
    //
    return;
  }

  Check(RegisterWormArenaEntity(arena, "heir", ARENA_LEASE, ARENA_TIMEOUT) == 0, "the slot of the peer is taken over");
  Check(arena->entity->number == 1, "the very number of the peer is handed out, got %u", arena->entity->number);

  pool   = FindWormArenaCollection(arena, "messages", WORM_COLLECTION_POOL);
  first  = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);
  second = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);

  Check(first  != NULL, "the block the peer has not taken is free");
  Check(second != NULL, "the block of the peer is not inherited along with its number");

  ReleaseWormArenaDescriptor(second);
  ReleaseWormArenaDescriptor(first);
  ReleaseWormArena(arena);

  free(hole);
}

// A peer that is gone holding a lock, whose very slot is then handed out again

static void RunLocker(uint8_t* memory)
{
  uint32_t generation;
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* region;

  hole  = MakeHole(memory, ARENA_LENGTH, 12);
  arena = CreateWormArena(hole, "locked");

  if ((arena == NULL) ||
      (RegisterWormArenaEntity(arena, "locker", 200, ARENA_TIMEOUT) != 0))
  {
    //
    _exit(1);
  }

  region = CreateWormArenaRegion(arena, "state", 4096, ARENA_TIMEOUT);

  if ((region == NULL) ||
      (LockWormArenaRegion(arena, region, WORM_LOCK_WRITE, ARENA_TIMEOUT, &generation) != 0))
  {
    //
    _exit(2);
  }

  // Gone with the lock held, and nobody to give it back
  _exit(0);
}

static void TestLock(uint8_t* memory)
{
  int status;
  pid_t child;
  uint32_t generation;
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* region;

  printf("lock\n");

  child = fork();

  if (child == 0)
  {
    //
    RunLocker(memory);
  }

  waitpid(child, &status, 0);

  Check(WIFEXITED(status) && (WEXITSTATUS(status) == 0), "the peer has taken the lock and gone");

  // Nobody registers until the lease has run out, so the number of the peer is the
  // first one free and the peer coming next inherits it

  usleep(400000);

  hole  = MakeHole(memory, ARENA_LENGTH, 13);
  arena = CreateWormArena(hole, "locked");

  Check(arena != NULL, "the arena of the peer is attached to");

  if (arena == NULL)
  {
    //
    return;
  }

  Check(RegisterWormArenaEntity(arena, "heir", ARENA_LEASE, ARENA_TIMEOUT) == 0, "the slot of the peer is taken over");
  Check(arena->entity->number == 1, "the very number of the peer is handed out, got %u", arena->entity->number);

  region = FindWormArenaCollection(arena, "state", WORM_COLLECTION_REGION);

  Check(region != NULL, "the region of the peer is found");
  Check(LockWormArenaRegion(arena, region, WORM_LOCK_WRITE, ARENA_TIMEOUT, &generation) == 0, "the lock of the dead peer is not held by its number any more");

  UnlockWormArenaRegion(region, WORM_LOCK_WRITE, generation);
  ReleaseWormArena(arena);

  free(hole);
}

// A producer that is gone holding a slot, whose number is then handed out again

static void RunProducer(uint8_t* memory)
{
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* pool;
  struct WormArenaCollection* queue;
  struct WormArenaDescriptor* descriptor;
  ATOMIC(uint64_t)* slots;

  hole  = MakeHole(memory, ARENA_LENGTH, 14);
  arena = CreateWormArena(hole, "queued");

  if ((arena == NULL) ||
      (RegisterWormArenaEntity(arena, "producer", 200, ARENA_TIMEOUT) != 0))
  {
    //
    _exit(1);
  }

  pool  = CreateWormArenaPool(arena, "messages", 2, POOL_LENGTH, ARENA_TIMEOUT);
  queue = CreateWormArenaQueue(arena, "events", 4, ARENA_TIMEOUT);

  if ((pool  == NULL) ||
      (queue == NULL))
  {
    //
    _exit(2);
  }

  // Take the first position the way a producer does and stop right there, before
  // the slot is filled in. This is the state a peer killed inside a push leaves

  slots = (ATOMIC(uint64_t)*)(memory + queue->offset);

  atomic_fetch_add_explicit(&queue->tail, 1, memory_order_relaxed);
  atomic_store_explicit(slots, WORM_SLOT_RESERVE(0, arena->entity->number), memory_order_release);

  // And behind that hole goes a message that is whole

  descriptor = CreateWormArenaDescriptor(arena, pool, NULL, 8, ARENA_TIMEOUT);

  if ((descriptor == NULL) ||
      (strcpy((char*)descriptor->data, "behind") == NULL) ||
      (PushWormArenaQueue(arena, queue, descriptor) != 0))
  {
    //
    _exit(3);
  }

  _exit(0);
}

static void TestQueueReuse(uint8_t* memory)
{
  int status;
  pid_t child;
  struct WormHole* hole;
  struct WormArena* arena;
  struct WormArenaCollection* queue;
  struct WormArenaDescriptor* descriptor;

  printf("queue reuse\n");

  child = fork();

  if (child == 0)
  {
    //
    RunProducer(memory);
  }

  waitpid(child, &status, 0);

  Check(WIFEXITED(status) && (WEXITSTATUS(status) == 0), "the producer has taken a slot and gone");

  usleep(400000);

  hole  = MakeHole(memory, ARENA_LENGTH, 15);
  arena = CreateWormArena(hole, "queued");

  Check(arena != NULL, "the arena of the producer is attached to");

  if (arena == NULL)
  {
    //
    return;
  }

  Check(RegisterWormArenaEntity(arena, "heir", ARENA_LEASE, ARENA_TIMEOUT) == 0, "the slot of the producer is taken over");
  Check(arena->entity->number == 1, "the very number of the producer is handed out, got %u", arena->entity->number);

  queue      = FindWormArenaCollection(arena, "events", WORM_COLLECTION_QUEUE);
  descriptor = PopWormArenaQueue(arena, queue);

  Check(queue      != NULL, "the queue of the producer is found");
  Check(descriptor != NULL, "the reservation of the dead producer does not hold the queue up");
  Check((descriptor != NULL) && (strcmp((char*)descriptor->data, "behind") == 0), "and the message behind it is whole");

  ReleaseWormArenaDescriptor(descriptor);
  ReleaseWormArena(arena);

  free(hole);
}

int main(int count, char** arguments)
{
  uint8_t* memory;

  memory = (uint8_t*)mmap(NULL, ARENA_LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  if (memory == MAP_FAILED)
  {
    perror("mmap");
    return 1;
  }

  TestArena(memory);

  memset(memory, 0, ARENA_LENGTH);

  TestPeers(memory);

  memset(memory, 0, ARENA_LENGTH);

  TestRecovery(memory);

  memset(memory, 0, ARENA_LENGTH);

  TestReuse(memory);

  memset(memory, 0, ARENA_LENGTH);

  TestLock(memory);

  memset(memory, 0, ARENA_LENGTH);

  TestQueueReuse(memory);

  printf("\n%s\n", failures ? "FAILED" : "PASSED");

  munmap(memory, ARENA_LENGTH);

  return failures != 0;
}
