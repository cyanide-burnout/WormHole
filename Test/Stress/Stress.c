#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>

#include "WormHole.h"

#define TRAP_COUNT    512
#define THREAD_LIMIT  64
#define PEER_LIMIT    64
#define KICK_ROUNDS   1000

struct Context
{
  struct WormHole* hole;
  atomic_int running;
  atomic_long kicks;
};

static int failures;
static int delay;

static __thread int kicking;

ssize_t write(int handle, const void* buffer, size_t size)
{
  if (kicking &&
      (delay > 0))
  {
    // Takes the place of write() of libc for WormHole.o as well, and widens the window between the moment
    // a kick has the number of an eventfd and the moment it writes to it, where a race would show up
    usleep(delay);
  }

  return syscall(SYS_write, handle, buffer, size);
}

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

static int CountDescriptors(void)
{
  struct dirent* entry;
  DIR* directory;
  int count;

  count     = -1;  // The directory itself
  directory = opendir("/proc/self/fd");

  while (entry = readdir(directory))
  {
    // Skip . and ..
    count += (entry->d_name[0] != '.');
  }

  closedir(directory);
  return count;
}

static void* KickPeers(void* closure)
{
  struct Context* context;
  uint32_t identifiers[PEER_LIMIT];
  uint32_t identifier;
  long count;
  int number;
  int round;
  int index;

  context = (struct Context*)closure;
  count   = 0;
  kicking = 1;

  while (atomic_load_explicit(&context->running, memory_order_relaxed))
  {
    number = 0;

    for (identifier = 0; (identifier <= UINT16_MAX) && (number < PEER_LIMIT); identifier ++)
    {
      if (atomic_load_explicit((_Atomic int*)context->hole->host.handles + identifier, memory_order_relaxed) >= 0)
      {
        // Peers live for a fraction of a millisecond, so the ones found now are the ones about to go
        identifiers[number ++] = identifier;
      }
    }

    for (round = 0; round < KICK_ROUNDS; round ++)
    {
      for (index = 0; index < number; index ++)
      {
        // A kick of a peer that has gone costs a load
        count += (KickWormHole(context->hole, identifiers[index]) == 0);
      }
    }
  }

  atomic_fetch_add_explicit(&context->kicks, count, memory_order_relaxed);
  return NULL;
}

int main(int count, const char** arguments)
{
  struct Context context;
  struct pollfd server;
  pthread_t threads[THREAD_LIMIT];
  int traps[TRAP_COUNT];
  uint64_t value;
  time_t moment;
  long events;
  long stray;
  int baseline;
  int expected;
  int actual;
  int number;
  int index;
  int live;
  int busy;
  int left;

  if (count < 3)
  {
    printf("%s <path to ivshmem socket> <seconds> [threads] [delay of a kick in microseconds]\n", arguments[0]);
    return EXIT_FAILURE;
  }

  number   = (count > 3) ? atoi(arguments[3]) : 4;
  number   = (number < 1) ? 1 : (number > THREAD_LIMIT) ? THREAD_LIMIT : number;
  delay    = (count > 4) ? atoi(arguments[4]) : 50;
  baseline = CountDescriptors();

  if ((context.hole = CreateWormHole(arguments[1], NULL, NULL)) == NULL)
  {
    printf("Error connecting to ivshmem-server at %s\n", arguments[1]);
    return EXIT_FAILURE;
  }

  atomic_init(&context.running, 1);
  atomic_init(&context.kicks, 0);

  for (index = 0; index < TRAP_COUNT; index ++)
  {
    // Not taken yet
    traps[index] = -1;
  }

  for (index = 0; index < number; index ++)
  {
    // Every thread kicks every peer it can find, over and over
    pthread_create(threads + index, NULL, KickPeers, &context);
  }

  server.fd     = context.hole->host.socket;
  server.events = POLLIN;
  moment        = time(NULL) + atoi(arguments[2]);
  events        = 0;
  stray         = 0;
  left          = 0;

  while (time(NULL) < moment)
  {
    if (poll(&server, 1, 100) > 0)
    {
      if (HandleWormHoleServerEvent(context.hole) <= 0)
      {
        // Peers are no longer coming and going, so nothing is being tested
        left = 1;
        break;
      }

      // The number of an eventfd closed just now is the lowest free one, so a trap takes it at once.
      // A kick that wrote into a closed eventfd after its number had been reused shows up in the trap
      index = events ++ % TRAP_COUNT;

      if ((traps[index] >= 0) &&
          (read(traps[index], &value, sizeof(uint64_t)) == sizeof(uint64_t)))
      {
        // The oldest trap is emptied before it is let go
        stray += value;
      }

      close(traps[index]);
      traps[index] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
  }

  atomic_store_explicit(&context.running, 0, memory_order_relaxed);

  for (index = 0; index < number; index ++)
  {
    // Kicks are over before anything is counted
    pthread_join(threads[index], NULL);
  }

  live = 0;
  busy = 0;

  for (index = 0; index < TRAP_COUNT; index ++)
  {
    if ((traps[index] >= 0) &&
        (read(traps[index], &value, sizeof(uint64_t)) == sizeof(uint64_t)))
    {
      // What is left in the traps is counted as well
      stray += value;
    }

    live += (traps[index] >= 0);
  }

  for (index = 0; index <= UINT16_MAX; index ++)
  {
    // A slot packs the number of an eventfd with the count of kicks writing to it and a bit for a peer that has gone.
    // Every eventfd that is not in a slot any more has been closed, and once the kicks are over no count is left
    // in a slot and no peer that has gone
    live += (context.hole->host.handles[index] >= 0);
    busy += (context.hole->host.handles[index] != -1) && (context.hole->host.handles[index] & 0xfff00000);
  }

  // Besides the eventfds and the traps, a hole keeps the socket, the shared memory and its own source
  expected = baseline + 2 + (context.hole->handle >= 0) + live;
  actual   = CountDescriptors();

  printf("%d threads, a kick delayed by %d us, %ld kicks delivered, %ld passes over messages of the server\n", number, delay, atomic_load(&context.kicks), events);

  Check(!left, "the server stays connected");
  Check(events > 0, "peers come and go, %ld passes", events);
  Check(stray == 0, "no kick reaches a descriptor that is no longer the eventfd of a peer, %ld did", stray);
  Check(busy == 0, "no slot is left counting kicks or holding a peer that has gone, %d are", busy);
  Check(actual == expected, "no descriptor is lost or closed twice, %d open where %d are expected", actual, expected);

  ReleaseWormHole(context.hole);

  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
