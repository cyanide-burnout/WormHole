#include <stdlib.h>
#include <stdio.h>

#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#include "FastRing.h"
#include "WormHole.h"
#include "WormLoop.h"

#ifdef TRACE
#include "FaultHandler.h"
#include "ReportTools.h"
#include "CXXTrace.h"
#endif

#define STATE_RUNNING  -1

struct Layout
{
  atomic_uint_least32_t count;
  atomic_uint_least32_t number;
  uint32_t peers[16];
  char message[256];
};

atomic_int state = { STATE_RUNNING };

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&state, EXIT_SUCCESS, memory_order_relaxed);
}

void HandleBell(struct WormHole* hole)
{
  struct Layout* layout;

  layout = (struct Layout*)hole->memory;

  printf("%s\n", layout->message);
}

void HandleTimeout(struct FastRingDescriptor* descriptor)
{
  struct Layout* layout;
  struct WormHole* hole;
  uint32_t number;

  hole   = (struct WormHole*)descriptor->closure;
  layout = (struct Layout*)hole->memory;
  number = atomic_fetch_add_explicit(&layout->number, 1, memory_order_relaxed);

  sprintf(layout->message, "Bell id=%d pid=%d count=%d        ", hole->identifier, getpid(), number);

  number = atomic_load_explicit(&layout->count, memory_order_acquire);

  while (number > 0)
  {
    number --;
    KickWormHole(hole, layout->peers[number]);
  }
}

int main(int count, const char** arguments)
{
  struct Layout* layout;
  struct FastRing* ring;
  struct WormHole* hole;
  struct WormLoop* loop;
  struct sigaction action;
  struct FastRingDescriptor* descriptor;

#ifdef TRACE
  SetFaultHandler(report, MakeFaultReportPrologue, MakeCXXTraceReport, MakeFaultProcessAbort, NULL);
#endif

  if (count < 2)
  {
    printf("%s <path to ivshmem socket ot UIO device>\n", arguments[0]);
    return EXIT_FAILURE;
  }

  hole = CreateWormHole(arguments[1], HandleBell, NULL);

  if (hole == NULL)
  {
    printf("Error openning ivshmem at %s\n", arguments[1]);
    return EXIT_FAILURE;
  }

  printf("Shared memory size: %lld\n", hole->length);

  //

  layout = (struct Layout*)hole->memory;
  layout->peers[atomic_fetch_add_explicit(&layout->count, 1, memory_order_relaxed) & 15] = hole->identifier;
  atomic_fetch_and_explicit(&layout->count, 15, memory_order_release);

  //

  ring       = CreateFastRing(0, NULL, NULL);
  loop       = CreateWormLoop(ring, hole);
  descriptor = SetFastRingTimeout(ring, NULL, 5000, TIMEOUT_FLAG_REPEAT, HandleTimeout, hole);

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  while ((atomic_load_explicit(&state, memory_order_relaxed) == STATE_RUNNING) &&
         (WaitFastRing(ring, 200, NULL) >= 0));

  SetFastRingTimeout(ring, descriptor, -1, 0, NULL, NULL);
  ReleaseWormLoop(loop);
  ReleaseFastRing(ring);
  ReleaseWormHole(hole);

  return EXIT_SUCCESS;
}