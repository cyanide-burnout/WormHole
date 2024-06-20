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
  struct FastRingDescriptor* descriptor;
};

struct WormLoop* CreateWormLoop(struct FastRing* ring, struct WormHole* hole);
void ReleaseWormLoop(struct WormLoop* loop);

#ifdef __cplusplus
}
#endif

#endif
