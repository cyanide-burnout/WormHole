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

static int KickHostWormHole(struct WormHole* hole, uint32_t identifier)
{
  struct WormLoop* loop;
  struct FastRingDescriptor* descriptor;

  if ((identifier <= UINT16_MAX) &&
      (hole->host.handles[identifier] >= 0) &&
      (loop       = (struct WormLoop*)hole->extension) &&
      (descriptor = AllocateFastRingDescriptor(loop->ring, NULL, NULL)))
  {
    descriptor->data.number = 1ULL;
    io_uring_prep_write(&descriptor->submission, hole->host.handles[identifier], &descriptor->data.number, sizeof(uint64_t), 0);
    SubmitFastRingDescriptor(descriptor, 0);
    return 0;
  }

  return -EBADF;
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
        if (loop->server = AddFastRingWatch(ring, hole->host.socket, POLLIN, 0, HandleHostServerEvent, loop))
        {
          hole->extension = loop;
          hole->kick      = KickHostWormHole;
        }
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
  struct WormHole* hole;

  if (loop != NULL)
  {
    if ((hole = loop->hole) &&
        (hole->extension == loop))
    {
      // Kick has to fail rather than to reach the released loop
      hole->extension = NULL;
      hole->kick      = NULL;
    }

    RemoveFastRingWatch(loop->server);
    RemoveFastRingWatch(loop->source);

    free(loop);
  }
}
