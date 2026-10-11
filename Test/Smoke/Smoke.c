#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#include "FastRing.h"
#include "WormHole.h"
#include "WormLoop.h"
#include "WormArena.h"

// Everything below runs over a real shared region of ivshmem, host mode and guest
// mode alike. Every peer has two ways of hearing from the others: a queue of its own
// that the rest put messages into by name, and one queue shared by everybody. The
// verdict stands on the first, because nobody competes for it; the second is there
// to be reported, because competing for one queue is where the interesting behaviour
// of an arena shows up

#define ARENA_NAME      "smoke"
#define ARENA_TIMEOUT    1000
#define ARENA_LEASE     10000

#define POOL_BLOCKS       128
#define POOL_LENGTH       256
#define QUEUE_SLOTS        64
#define INBOX_SLOTS        32
#define REGION_LENGTH    4096

#define TICK             1000
#define SENDERS            16
#define PORTION             1
#define SETTLE              8
#define STATE_RUNNING      -1

// An inbox is named after the slot of its owner and not after its name. A collection
// cannot be deleted once it is laid out, so a name of a peer would burn a slot of the
// table for good and a run of enough peers would exhaust it; there are only ever as
// many slots for entities as there are, and an inbox is reused along with one. It
// also keeps the name short: what a collection carries is 64 bytes and no more, and
// a peer with a long name would have laid out an inbox nobody could find by name

#define INBOX_NAME(buffer, number)  snprintf(buffer, sizeof(buffer), "inbox-%u", (unsigned)(number))

struct State
{
  uint32_t turns;
  char holder[64];
};

struct Context
{
  struct WormArena* arena;
  struct WormArenaCollection* pool;
  struct WormArenaCollection* queue;
  struct WormArenaCollection* inbox;
  struct WormArenaCollection* region;
  const char* name;
  uint32_t sent;
  uint32_t posted;
  uint32_t refused;
  uint32_t taken;
  uint32_t shared;
  uint32_t stalled;
  uint32_t bells;
  uint32_t turns;
  uint32_t missed;
  uint32_t lapsed;
  uint32_t quiet;
  uint32_t count;
  char sender[SENDERS][64];
};

static atomic_int running = { STATE_RUNNING };

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

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&running, EXIT_SUCCESS, memory_order_relaxed);
}

static uint64_t GetTime()
{
  struct timespec value;

  clock_gettime(CLOCK_REALTIME, &value);

  return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static void NoteSender(struct Context* context, const char* message)
{
  uint32_t index;
  size_t length;

  // A message carries the name of its sender ahead of the first space
  length = strcspn(message, " ");
  length = length < sizeof(context->sender[0]) ? length : sizeof(context->sender[0]) - 1;

  for (index = 0; index < context->count; index ++)
  {
    if ((strncmp(context->sender[index], message, length) == 0) &&
        (context->sender[index][length] == '\0'))
    {
      //
      return;
    }
  }

  if (context->count < SENDERS)
  {
    memcpy(context->sender[context->count], message, length);
    context->sender[context->count][length] = '\0';
    context->count ++;
  }
}

static void Discard(struct WormArena* arena, struct WormArenaCollection* queue)
{
  struct WormArenaDescriptor* descriptor;

  // A slot of an entity comes back around, and so does the inbox named after it, so
  // whatever the peer before us never took is given back to the pool here

  while (descriptor = PopWormArenaQueue(arena, queue))
  {
    //
    ReleaseWormArenaDescriptor(descriptor);
  }
}

static void TakeInbox(struct Context* context)
{
  struct WormArenaDescriptor* descriptor;

  // Nobody else takes from here, so it is emptied without a second thought

  while (descriptor = PopWormArenaQueue(context->arena, context->inbox))
  {
    NoteSender(context, (const char*)descriptor->data);
    ReleaseWormArenaDescriptor(descriptor);
    context->taken ++;
  }
}

static void TakeQueue(struct Context* context, uint32_t portion)
{
  struct WormArenaDescriptor* descriptor;
  ATOMIC(uint64_t)* slots;
  uint64_t value;

  // One message per wakeup and no more. A bell only says that something may have
  // arrived, and whoever grabs a batch on every bell starves the slower participants:
  // a peer of the host, which wakes without leaving its machine, was measured taking
  // two thirds of everything while a peer of a guest took none

  while (portion != 0)
  {
    if (descriptor = PopWormArenaQueue(context->arena, context->queue))
    {
      ReleaseWormArenaDescriptor(descriptor);
      context->shared ++;
      portion --;
      continue;
    }

    // Taking has failed, and an empty queue is only one of the two reasons for it: a
    // slot that a producer has claimed and not filled in yet holds the whole queue up
    // until it does, and the two are worth telling apart

    slots = (ATOMIC(uint64_t)*)(context->arena->hole->memory + context->queue->offset);
    value = atomic_load_explicit(slots + (context->queue->head & (context->queue->count - 1)), memory_order_acquire);

    context->stalled += (WORM_SLOT_RESERVED(value) != 0);
    return;
  }
}

static void HandleEvent(struct WormHole* hole, int event, int value)
{
  struct Context* context;

  if ((event   == HOLE_EVENT_BELL) &&
      (context = (struct Context*)hole->closure) &&
      (context->arena != NULL))
  {
    context->bells ++;

    // Taking from a queue waits for nothing, so a bell is a fair place for it
    TakeInbox(context);
    TakeQueue(context, PORTION);
  }
}

static int Post(struct Context* context, struct WormArenaCollection* queue, const char* text)
{
  struct WormArenaDescriptor* message;
  int result;

  result = -1;

  if (message = CreateWormArenaDescriptor(context->arena, context->pool, NULL, POOL_LENGTH, ARENA_TIMEOUT))
  {
    snprintf((char*)message->data, POOL_LENGTH, "%s", text);
    result = PushWormArenaQueue(context->arena, queue, message);
    ReleaseWormArenaDescriptor(message);
  }

  context->refused += (result != 0);

  return result;
}

static void HandleTimeout(struct FastRingDescriptor* descriptor)
{
  struct Context* context;
  struct WormArenaMagic* magic;
  struct WormArenaEntity* entity;
  struct WormArenaCollection* inbox;
  struct State* state;
  uint32_t generation;
  uint32_t index;
  uint64_t time;
  char text[POOL_LENGTH];
  char name[32];

  context = (struct Context*)descriptor->closure;
  magic   = (struct WormArenaMagic*)context->arena->hole->memory;

  if (TouchWormArena(context->arena, ARENA_LEASE, ARENA_TIMEOUT) != 0)
  {
    // An entity whose lease has run out is a dead one as far as everybody else is
    // concerned, and whatever it was holding is given back under it. Nothing else is
    // done in its name this round: taking would put its hold on a block, pushing would
    // claim a slot of a queue by its number, and the region would be locked by a number
    // that a sweep is free to hand to somebody else

    context->lapsed ++;
    return;
  }

  // Taking comes before putting on purpose: a peer that drained right after its own
  // push would find the shared queue holding nothing but the message it had just
  // made, and would go on eating its own tail while never hearing anybody

  TakeInbox(context);
  TakeQueue(context, PORTION);

  snprintf(text, sizeof(text), "%s %u", context->name, context->posted);
  time = GetTime();

  for (index = 0; index < WORM_ARENA_ENTITIES; index ++)
  {
    entity = magic->entities + index;

    // A neighbour on its way out is alive and still holds what it holds, it is only
    // not to be given anything new

    if ((atomic_load_explicit(&entity->time, memory_order_acquire) >= time)                          &&
        (~WORM_ENTITY_STATE(atomic_load_explicit(&entity->state, memory_order_acquire)) & WORM_ENTITY_QUIESCING) &&
        (entity->name[0] != '\0')                                                                    &&
        (strcmp(entity->name, context->name) != 0))
    {
      // A peer that is there but whose inbox is not laid out yet is simply skipped,
      // it will be there by the next turn

      INBOX_NAME(name, index);

      if ((inbox = FindWormArenaCollection(context->arena, name, WORM_COLLECTION_QUEUE)) &&
          (Post(context, inbox, text) == 0))
      {
        //
        context->sent ++;
      }
    }
  }

  if (Post(context, context->queue, text) == 0)
  {
    //
    context->posted ++;
  }

  if (LockWormArenaRegion(context->arena, context->region, WORM_LOCK_WRITE, ARENA_TIMEOUT, &generation) == 0)
  {
    state = (struct State*)GetWormArenaRegionData(context->arena, context->region);

    state->turns ++;
    snprintf(state->holder, sizeof(state->holder), "%s", context->name);

    UnlockWormArenaRegion(context->region, WORM_LOCK_WRITE, generation);
    context->turns ++;
  }
  else
  {
    //
    context->missed ++;
  }

  KickAllWormArenaEntities(context->arena);
}

int main(int count, const char** arguments)
{
  struct Context context;
  struct FastRing* ring;
  struct WormHole* hole;
  struct WormLoop* loop;
  struct FastRingDescriptor* descriptor;
  struct sigaction action;
  struct State* state;
  uint32_t generation;
  uint32_t index;
  char name[64];
  char title[32];
  struct timespec pause;
  time_t moment;
  int seconds;
  int peers;

  setvbuf(stdout, NULL, _IOLBF, 0);
  memset(&context, 0, sizeof(context));

  if (count < 2)
  {
    printf("%s <path to ivshmem socket or UIO device> [name] [seconds] [peers]\n", arguments[0]);
    printf("  peers is how many take part in the run, this one included, and the run\n");
    printf("  says nothing good unless it has heard from every one of the others\n");
    return EXIT_FAILURE;
  }

  if ((count > 2) &&
      (arguments[2][0] != '\0'))
  {
    //
    snprintf(name, sizeof(name), "%s", arguments[2]);
  }
  else
  {
    memset(name, 0, sizeof(name));
    gethostname(name, sizeof(name) - 8);
    snprintf(name + strlen(name), sizeof(name) - strlen(name), "-%d", getpid());
  }

  context.name = name;
  seconds      = (count > 3) ? atoi(arguments[3]) : 20;
  peers        = (count > 4) ? atoi(arguments[4]) : 1;

  if ((peers < 1) ||
      (peers > SENDERS + 1))
  {
    // Fewer than one is not a run at all, and more than the table of senders can hold
    // could never be satisfied however well everything worked
    printf("The number of peers has to be between 1 and %d\n", SENDERS + 1);
    return EXIT_FAILURE;
  }

  if ((hole = CreateWormHole(arguments[1], HandleEvent, &context)) == NULL)
  {
    printf("Error opening ivshmem at %s\n", arguments[1]);
    return EXIT_FAILURE;
  }

  printf("peer %s, region of %lld bytes, identifier %llu\n", name, (long long)hole->length, (unsigned long long)hole->identifier);

  if ((context.arena = CreateWormArena(hole, ARENA_NAME)) == NULL)
  {
    printf("Error laying out or attaching the arena\n");
    ReleaseWormHole(hole);
    return EXIT_FAILURE;
  }

  if (RegisterWormArenaEntity(context.arena, name, ARENA_LEASE, ARENA_TIMEOUT) != 0)
  {
    printf("Error registering the entity\n");
    ReleaseWormArena(context.arena);
    ReleaseWormHole(hole);
    return EXIT_FAILURE;
  }

  // Whoever gets there first lays the shared ones out and the rest attach to them by
  // name. The inbox belongs to this peer alone and is named after it

  INBOX_NAME(title, context.arena->entity->number);

  context.pool   = CreateWormArenaPool(context.arena, "messages", POOL_BLOCKS, POOL_LENGTH, ARENA_TIMEOUT);
  context.queue  = CreateWormArenaQueue(context.arena, "events", QUEUE_SLOTS, ARENA_TIMEOUT);
  context.inbox  = CreateWormArenaQueue(context.arena, title, INBOX_SLOTS, ARENA_TIMEOUT);
  context.region = CreateWormArenaRegion(context.arena, "state", REGION_LENGTH, ARENA_TIMEOUT);

  if ((context.pool   == NULL) ||
      (context.queue  == NULL) ||
      (context.inbox  == NULL) ||
      (context.region == NULL))
  {
    printf("Error creating the collections\n");
    ReleaseWormArena(context.arena);
    ReleaseWormHole(hole);
    return EXIT_FAILURE;
  }

  Discard(context.arena, context.inbox);

  ring       = CreateFastRing(0);
  loop       = CreateWormLoop(ring, hole);
  descriptor = SetFastRingTimeout(ring, NULL, TICK, TIMEOUT_FLAG_REPEAT, HandleTimeout, &context);

  if ((ring       == NULL) ||
      (loop       == NULL) ||
      (descriptor == NULL))
  {
    // A loop that was not attached would leave the doorbell unheard, and a peer that
    // cannot hear it would still take its own messages back and call that a success
    printf("Error building the event loop: ring %p, loop %p, timeout %p\n", (void*)ring, (void*)loop, (void*)descriptor);
    ReleaseWormLoop(loop);
    ReleaseFastRing(ring);
    ReleaseWormArena(context.arena);
    ReleaseWormHole(hole);
    return EXIT_FAILURE;
  }

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  moment = time(NULL) + seconds;

  while ((atomic_load_explicit(&running, memory_order_relaxed) == STATE_RUNNING) &&
         (time(NULL) < moment) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));

  SetFastRingTimeout(ring, descriptor, -1, 0, NULL, NULL);
  ReleaseWormLoop(loop);
  ReleaseFastRing(ring);

  // The word goes out first and the emptying follows it. Whoever looks at this peer
  // from now on leaves it alone, and CheckWormArenaQuiescence says when the last of
  // those who could still have been addressing it has finished the pass it was in --
  // by the rounds of the others and not by a clock. The lease is held through all of
  // it: taking from a queue puts a hold on the block behind the message in the name of
  // this entity, and a hold of a peer everybody else counts as dead is what the next
  // sweep takes back from under a descriptor still in hand. The entity dies at
  // ReleaseWormArena and not a moment sooner

  QuiesceWormArena(context.arena, ARENA_TIMEOUT);

  pause.tv_sec  = 0;
  pause.tv_nsec = TICK * 1000000L / 4;

  for (index = 0; index < SETTLE; index ++)
  {
    if (TouchWormArena(context.arena, ARENA_LEASE, ARENA_TIMEOUT) != 0)
    {
      // The lease is gone and so, to everybody else, is this peer. Taking from a queue
      // now would put the hold of a dead entity on a block, for the next sweep to take
      // back from under a descriptor still in hand

      context.lapsed ++;
      break;
    }

    TakeInbox(&context);
    TakeQueue(&context, QUEUE_SLOTS);

    if (CheckWormArenaQuiescence(context.arena, ARENA_TIMEOUT) == 0)
    {
      // Nobody is addressing this peer any more, so what is in the inbox is all that
      // will ever be in it

      TakeInbox(&context);
      context.quiet = 1;
      break;
    }

    nanosleep(&pause, NULL);
  }

  printf("\n");
  printf("  inbox    %u sent, %u taken from %u senders:", context.sent, context.taken, context.count);

  for (index = 0; index < context.count; index ++)
  {
    //
    printf(" %s", context.sender[index]);
  }

  printf("\n");
  printf("  queue    %u posted, %u taken, %u refused, %u stalled on a claimed slot\n", context.posted, context.shared, context.refused, context.stalled);
  printf("  region   %u turns, %u missed\n", context.turns, context.missed);
  printf("  lease    %u extensions refused\n", context.lapsed);
  printf("  going    %s\n", context.quiet ? "quiet before leaving" : "still spoken to when it left");
  printf("  bells    %u\n\n", context.bells);

  Check(context.posted != 0, "blocks are taken out of the pool and pushed, %u of them", context.posted);
  Check(context.turns  != 0, "the write lock of the region is taken, %u times", context.turns);
  Check(context.missed == 0, "and never missed, %u missed", context.missed);
  Check(context.lapsed == 0, "the lease is held for the whole run, %u extensions refused", context.lapsed);
  Check(context.quiet  != 0, "the departure is quiet before it is made, nobody left addressing this peer");

  // Nobody competes for an inbox, so hearing from every other peer is a matter of the
  // arena working and not of winning a race for the head of a queue somebody shares

  Check(context.count >= (uint32_t)(peers - 1), "every one of the %d other peers is heard in the inbox, heard %u", peers - 1, context.count);

  if (LockWormArenaRegion(context.arena, context.region, WORM_LOCK_READ, ARENA_TIMEOUT, &generation) == 0)
  {
    state = (struct State*)GetWormArenaRegionData(context.arena, context.region);
    Check(state->turns >= context.turns, "the region carries the turns of everybody, %u of them, last held by %s", state->turns, state->holder);
    UnlockWormArenaRegion(context.region, WORM_LOCK_READ, generation);
  }
  else
  {
    //
    Check(0, "the region can be read at the end of the run");
  }

  ReleaseWormArena(context.arena);
  ReleaseWormHole(hole);

  printf("\n%s\n", failures ? "FAILED" : "PASSED");
  return failures != 0;
}
