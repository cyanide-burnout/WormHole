#include "WormLoop.h"

#include <malloc.h>

static int HandleInterruptEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct WormLoop* loop;

  if (( completion != NULL) &&
      (~completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    loop = (struct WormLoop*)descriptor->closure;
    HandleWormHoleInterruptEvent(loop->hole, (int64_t)descriptor->data.number);
    SubmitFastRingDescriptor(descriptor, 0);
    return 1;
  }

  return 0;
}

static void HandleHostServerEvent(int handle, uint32_t flags, void* closure, uint64_t options)
{
  struct WormLoop* loop;
  struct WormHole* hole;
  struct FastRingDescriptor* descriptor;

  loop = (struct WormLoop*)closure;
  hole = loop->hole;

  HandleWormHoleHostServerEvent(loop->hole);

  if ((loop->descriptor == NULL) &&
      (hole->handle     >= 0))
  {
    descriptor = loop->descriptor = AllocateFastRingDescriptor(loop->ring, HandleInterruptEvent, loop);
    io_uring_prep_read(&descriptor->submission, hole->handle, &descriptor->data.number, hole->size, 0);
    SubmitFastRingDescriptor(descriptor, 0);
  }
}

static int KickHostWormHole(struct WormHole* hole, uint32_t identifier)
{
  struct WormLoop* loop;
  struct FastRingDescriptor* descriptor;

  if ((hole->host.handles[identifier] >= 0) &&
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
  struct FastRingDescriptor* descriptor;

  if ((ring == NULL) ||
      (hole == NULL))
  {
    //
    return NULL;
  }

  loop       = (struct WormLoop*)calloc(1, sizeof(struct WormLoop));
  loop->ring = ring;
  loop->hole = hole;

  switch (hole->type)
  {
    case HOLE_CLIENT_TYPE_HOST:
      hole->extension = loop;
      hole->kick      = KickHostWormHole;
      AddFastRingEventHandler(ring, hole->host.socket, POLLIN | POLLHUP, HandleHostServerEvent, loop);
      break;

    case HOLE_CLIENT_TYPE_GUEST:
      descriptor = loop->descriptor = AllocateFastRingDescriptor(loop->ring, HandleInterruptEvent, loop);
      io_uring_prep_read(&descriptor->submission, hole->handle, &descriptor->data.number, hole->size, 0);
      SubmitFastRingDescriptor(descriptor, 0);
      break;
  }

  return loop;
}

void ReleaseWormLoop(struct WormLoop* loop)
{
  struct WormHole* hole;
  struct FastRingDescriptor* descriptor;

  if (loop != NULL)
  {
    if ((hole = loop->hole) &&
        (hole->type == HOLE_CLIENT_TYPE_HOST))
    {
      // There are no frequent events, poll interface is enough
      RemoveFastRingEventHandler(loop->ring, hole->host.socket);
    }

    if ((descriptor = loop->descriptor) &&
        (descriptor->state == RING_DESC_STATE_PENDING))
    {
      descriptor->submission.opcode     = IORING_OP_NOP;
      descriptor->submission.user_data |= RING_DESC_OPTION_IGNORE;
      loop->descriptor                  = NULL;
    }

    if (descriptor = loop->descriptor)
    {
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_prep_cancel(&descriptor->submission, descriptor, 0);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    }

    free(loop);
  }
}
