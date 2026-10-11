#include "WormLoop.h"

#include <malloc.h>

#define POLL_EVENT_LOST  (POLLERR | POLLHUP | POLLNVAL)

static void HandleInterruptEvent(struct FastRingDescriptor* descriptor, int result)
{
  struct WormLoop* loop;

  loop = (struct WormLoop*)descriptor->closure;

  if ((result > 0) &&
      (result & POLLIN))
  {
    // Value is read by the handler itself, watch reports readiness only
    HandleWormHoleInterruptEvent(loop->hole, HOLE_CLIENT_VALUE_READ);
  }

  if ((result < 0) ||
      (result & POLL_EVENT_LOST))
  {
    // Event source is gone, there is nothing to wait for anymore
    loop->source = NULL;
    RemoveFastRingWatch(descriptor);
    HandleWormHoleLossEvent(loop->hole);
  }
}

static void HandleHostServerEvent(struct FastRingDescriptor* descriptor, int result)
{
  struct WormLoop* loop;
  struct WormHole* hole;
  int status;

  loop   = (struct WormLoop*)descriptor->closure;
  hole   = loop->hole;
  status = (result < 0) || (result & POLL_EVENT_LOST);

  if ((result > 0) &&
      (result & POLLIN))
  {
    // Messages are drained until the socket buffer gets empty
    status |= (HandleWormHoleHostServerEvent(hole) <= 0);

    if ((loop->source == NULL) &&
        (hole->handle >= 0))
    {
      // Server has just granted an eventfd of the peer itself, and without a watch on it no bell is heard again
      loop->source  = AddFastRingWatch(loop->ring, hole->handle, POLLIN, 0, HandleInterruptEvent, loop);
      status       |= (loop->source == NULL);
    }
  }

  if (status)
  {
    // Connection to ivshmem-server is gone, there is nothing to expect anymore
    loop->server = NULL;
    RemoveFastRingWatch(descriptor);
    HandleWormHoleLossEvent(hole);
  }
}

struct WormLoop* CreateWormLoop(struct FastRing* ring, struct WormHole* hole)
{
  struct WormLoop* loop;

  loop = NULL;

  if ((ring != NULL) &&
      (hole != NULL) &&
      (loop  = (struct WormLoop*)calloc(1, sizeof(struct WormLoop))))
  {
    loop->ring = ring;
    loop->hole = hole;

    switch (hole->type)
    {
      case HOLE_CLIENT_TYPE_HOST:
        // Own eventfd is not granted yet, event source is watched later
        loop->server = AddFastRingWatch(ring, hole->host.socket, POLLIN, 0, HandleHostServerEvent, loop);
        break;

      case HOLE_CLIENT_TYPE_GUEST:
        // UIO device has no support of NOWAIT reads, watch keeps io-wq out of the way
        loop->source = AddFastRingWatch(ring, hole->handle, POLLIN, 0, HandleInterruptEvent, loop);
        break;
    }

    if ((loop->server == NULL) &&
        (loop->source == NULL))
    {
      // Exactly one of the two is expected by now, and a loop that watches nothing
      // would keep silent instead of failing, which is worse than not being created
      free(loop);
      loop = NULL;
    }
  }

  return loop;
}

void ReleaseWormLoop(struct WormLoop* loop)
{
  if (loop != NULL)
  {
    RemoveFastRingWatch(loop->server);
    RemoveFastRingWatch(loop->source);

    free(loop);
  }
}
