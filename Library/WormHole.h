#ifndef WORMHOLE_H
#define WORMHOLE_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HOLE_CLIENT_TYPE_GUEST  1
#define HOLE_CLIENT_TYPE_HOST   2

#define HOLE_CLIENT_VALUE_READ  INT64_MIN

#define HOLE_EVENT_READY        0  // Host mode only: the server has granted the event source, value is the ID of this peer
#define HOLE_EVENT_LOST         1  // Value is HOLE_LOST_SERVER or HOLE_LOST_SOURCE
#define HOLE_EVENT_BELL         2  // Value is 0

#define HOLE_LOST_SERVER        0
#define HOLE_LOST_SOURCE        1

struct WormHole;

typedef void (*HandleWormHoleEventFunction)(struct WormHole* hole, int event, int value);
typedef void (*ReleaseWormHoleFunction)(struct WormHole* hole);
typedef int (*KickWormHoleFunction)(struct WormHole* hole, uint32_t identifier);

struct GuestWormHole
{
  uint32_t* registers;  // Pointer to BAR0 of ivshmem
};

struct HostWormHole
{
  int socket;                   // Handle of server's connection
  int memory;                   // Handle of shared memory
  int handles[UINT16_MAX + 1];  // Handles of eventfd for all peers
};

struct WormHole
{
  int type;
  void* closure;
  void* extension;
  KickWormHoleFunction kick;
  ReleaseWormHoleFunction release;
  HandleWormHoleEventFunction event;

  uint64_t identifier;  // Local peer ID
  uint8_t* memory;      // Pointer to shared memory
  size_t length;        // Size of available shared memory
  size_t size;          // Size of data to read from event source
  int handle;           // Handle of event source

  union
  {
    struct HostWormHole host;
    struct GuestWormHole guest;
  };
};

#define IsWormHoleReady(hole)  ((hole)->handle >= 0)

struct WormHole* CreateWormHole(const char* path, HandleWormHoleEventFunction function, void* closure);

int HandleWormHoleServerEvent(struct WormHole* hole);
void HandleWormHoleBellEvent(struct WormHole* hole, int64_t value);
void HandleWormHoleLossEvent(struct WormHole* hole, int value);

static inline __attribute__((always_inline)) int KickWormHole(struct WormHole* hole, uint32_t identifier)
{
  if ((hole != NULL) &&
      (hole->kick != NULL))
  {
    // Be sure it is safe
    return hole->kick(hole, identifier);
  }

  return -EBADF;
}

static inline __attribute__((always_inline)) void ReleaseWormHole(struct WormHole* hole)
{
  if ((hole != NULL) &&
      (hole->release != NULL))
  {
    // Be sure it is safe
    hole->release(hole);
  }
}

#ifdef __cplusplus
}
#endif

#endif
