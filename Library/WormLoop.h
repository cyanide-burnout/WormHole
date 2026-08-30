#ifndef WORMLOOP_H
#define WORMLOOP_H

#include "WormHole.h"
#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct WormLoop
{
  struct WormHole* hole;
  struct FastRing* ring;
  struct FastRingDescriptor* source;  // Watch of event source delivering the bell
  struct FastRingDescriptor* server;  // Watch of connection to ivshmem-server (host mode only)
};

struct WormLoop* CreateWormLoop(struct FastRing* ring, struct WormHole* hole);
void ReleaseWormLoop(struct WormLoop* loop);

#ifdef __cplusplus
}
#endif

#endif
