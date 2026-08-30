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
// mode alike, and every peer is both a producer and a consumer of the same queue

#define ARENA_NAME      "smoke"
#define ARENA_TIMEOUT    1000
#define ARENA_LEASE     10000

#define POOL_BLOCKS        64
#define POOL_LENGTH       256
#define QUEUE_SLOTS        64
#define REGION_LENGTH    4096

#define TICK             1000
#define SENDERS            16
#define PORTION             8
#define STATE_RUNNING      -1

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
  struct WormArenaCollection* region;
  const char* name;
  uint32_t sent;
  uint32_t taken;
  uint32_t refused;
  uint32_t bells;
  uint32_t turns;
  uint32_t missed;
  uint32_t lapsed;
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

static void Drain(struct Context* context)
{
  struct WormArenaDescriptor* descriptor;
  uint32_t portion;

  // One doorbell wakes every process of a machine at once, so a consumer that emptied
  // the queue on every wakeup would leave its neighbours nothing and hold up the loop
  // besides. What is left over comes with the next bell

  portion = PORTION;

  while ((portion != 0) &&
         (descriptor = PopWormArenaQueue(context->arena, context->queue)))
  {
    NoteSender(context, (const char*)descriptor->data);
    ReleaseWormArenaDescriptor(descriptor);
    context->taken ++;
    portion --;
  }
}

static void HandleBell(struct WormHole* hole)
{
  struct Context* context;

  if (context = (struct Context*)hole->closure)
  {
    context->bells ++;

    if (context->arena != NULL)
    {
      // Taking from a queue waits for nothing, so a bell is a fair place for it
      Drain(context);
    }
  }
}

static void HandleTimeout(struct FastRingDescriptor* descriptor)
{
  struct Context* context;
  struct WormArenaDescriptor* message;
  struct State* state;
  uint32_t generation;
  char text[POOL_LENGTH];

  context = (struct Context*)descriptor->closure;

  if (TouchWormArena(context->arena, ARENA_LEASE, ARENA_TIMEOUT) != 0)
  {
    // An entity whose lease has run out is a dead one as far as everybody else is
    // concerned, and whatever it was holding is given back under it
    context->lapsed ++;
  }

  snprintf(text, sizeof(text), "%s %u", context->name, context->sent);

  if (message = CreateWormArenaDescriptor(context->arena, context->pool, NULL, sizeof(text), ARENA_TIMEOUT))
  {
    strcpy((char*)message->data, text);

    if (PushWormArenaQueue(context->arena, context->queue, message) == 0)
      context->sent ++;
    else
      context->refused ++;

    ReleaseWormArenaDescriptor(message);
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
  Drain(context);
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
  time_t moment;
  int seconds;
  int peers;

  setvbuf(stdout, NULL, _IOLBF, 0);
  memset(&context, 0, sizeof(context));

  if (count < 2)
  {
    printf("%s <path to ivshmem socket or UIO device> [name] [seconds] [peers]\n", arguments[0]);
    printf("  peers is the least number of distinct senders this run has to hear from,\n");
    printf("  itself included, so a lonely peer taking its own messages back is not\n");
    printf("  mistaken for a peer that has actually reached somebody\n");
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

  if ((hole = CreateWormHole(arguments[1], HandleBell, &context)) == NULL)
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

  // Whoever gets there first lays these out and the rest attach to them by name

  context.pool   = CreateWormArenaPool(context.arena, "messages", POOL_BLOCKS, POOL_LENGTH, ARENA_TIMEOUT);
  context.queue  = CreateWormArenaQueue(context.arena, "events", QUEUE_SLOTS, ARENA_TIMEOUT);
  context.region = CreateWormArenaRegion(context.arena, "state", REGION_LENGTH, ARENA_TIMEOUT);

  if ((context.pool   == NULL) ||
      (context.queue  == NULL) ||
      (context.region == NULL))
  {
    printf("Error creating the collections\n");
    ReleaseWormArena(context.arena);
    ReleaseWormHole(hole);
    return EXIT_FAILURE;
  }

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

  printf("\n");
  printf("  sent     %u\n", context.sent);
  printf("  taken    %u\n", context.taken);
  printf("  refused  %u\n", context.refused);
  printf("  bells    %u\n", context.bells);
  printf("  turns    %u taken, %u missed\n", context.turns, context.missed);
  printf("  lease    %u extensions refused\n", context.lapsed);
  printf("  senders  %u:", context.count);

  for (index = 0; index < context.count; index ++)
  {
    //
    printf(" %s", context.sender[index]);
  }

  printf("\n\n");

  Check(context.sent  != 0, "blocks are taken out of the pool and pushed, %u of them", context.sent);
  Check(context.taken != 0, "messages come back out of the queue, %u of them", context.taken);
  Check(context.turns != 0, "the write lock of the region is taken, %u times", context.turns);
  Check(context.missed == 0, "and never missed, %u missed", context.missed);
  Check(context.lapsed == 0, "the lease is held for the whole run, %u extensions refused", context.lapsed);

  // A lonely peer takes its own messages back and would call that a success, so the
  // run has to be told how many distinct senders it is supposed to end up hearing

  Check(context.count >= (uint32_t)peers, "at least %d distinct senders are heard, heard %u", peers, context.count);

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
