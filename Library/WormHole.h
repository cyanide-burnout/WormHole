#ifndef WORMHOLE_H
#define WORMHOLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HOLE_CLIENT_TYPE_GUEST  1
#define HOLE_CLIENT_TYPE_HOST   2

#define HOLE_CLIENT_VALUE_READ  INT64_MIN

struct WormHole;

typedef void (*HandleWormHoleBellFunction)(struct WormHole* hole);
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
  HandleWormHoleBellFunction bell;

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

struct WormHole* CreateWormHole(const char* path, HandleWormHoleBellFunction function, void* closure);

int HandleWormHoleHostServerEvent(struct WormHole* hole);
void HandleWormHoleInterruptEvent(struct WormHole* hole, int64_t value);

inline __attribute__((always_inline)) int KickWormHole(struct WormHole* hole, uint32_t identifier)
{
  if ((hole != NULL) &&
      (hole->release != NULL))
  {
    // Be sure it is safe
    hole->kick(hole, identifier);
  }
}

inline __attribute__((always_inline)) void ReleaseWormHole(struct WormHole* hole)
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
