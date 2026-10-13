#include "WormHole.h"

#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include <inttypes.h>
#include <stdatomic.h>

#include <fcntl.h>
#include <limits.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>

#include <stdio.h>

// Host mode client

#define IVSHMEM_PROTOCOL_VERSION  0

// A slot of the table of peers packs the number of an eventfd with the count of kicks writing to it,
// so that the eventfd of a peer that has gone is closed by whoever stops using it last

#define HOST_HANDLE_EMPTY  ((uint32_t)-1)  // Peer is not known
#define HOST_HANDLE_MASK   0x000fffffU     // Number of the eventfd, below the default of fs.nr_open
#define HOST_HANDLE_USER   0x00100000U     // One kick writing to the eventfd right now
#define HOST_HANDLE_BUSY   0x7ff00000U     // Count of kicks writing to the eventfd right now
#define HOST_HANDLE_GONE   0x80000000U     // Peer has gone

static int ReadHostServerMessage(struct WormHole* hole, int64_t* number, int* handle, int flags)
{
  int result;
  struct iovec vector;
  struct msghdr message;
  struct cmsghdr* header;
  char control[CMSG_SPACE(sizeof(int))];

  vector.iov_base        = number;
  vector.iov_len         = sizeof(int64_t);
  message.msg_name       = NULL;
  message.msg_namelen    = 0;
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control;
  message.msg_controllen = sizeof(control);
  message.msg_flags      = 0;

  result  = recvmsg(hole->host.socket, &message, flags);
  *number = le64toh(*number);

  if ((result > 0) &&
      (handle != NULL))
  {
    for (header = CMSG_FIRSTHDR(&message); header != NULL; header = CMSG_NXTHDR(&message, header))
    {
      if ((header->cmsg_len   == CMSG_LEN(sizeof(int))) &&
          (header->cmsg_level == SOL_SOCKET) &&
          (header->cmsg_type  == SCM_RIGHTS))
      {
        *handle = *(int*)CMSG_DATA(header);
        break;
      }
    }
  }

  return result;
}

static int KickHostWormHole(struct WormHole* hole, uint32_t identifier)
{
  _Atomic uint32_t* slot;
  uint64_t value;
  uint32_t state;
  int result;

  if (identifier > UINT16_MAX)
  {
    // There is no such peer
    return -EBADF;
  }

  slot = (_Atomic uint32_t*)hole->host.handles + identifier;

  // Count this kick in as one writing to the eventfd: while it is written to nobody closes it, and its number
  // cannot be reused for something else meanwhile. Only a count that is full waits for a write of another thread
  while ((((state = atomic_load_explicit(slot, memory_order_acquire)) & HOST_HANDLE_GONE) == 0) &&
         (((state & HOST_HANDLE_BUSY) == HOST_HANDLE_BUSY) ||
          (!atomic_compare_exchange_weak_explicit(slot, &state, state + HOST_HANDLE_USER, memory_order_acquire, memory_order_relaxed))));

  if (state & HOST_HANDLE_GONE)
  {
    // Peer is not known or has gone
    return -EBADF;
  }

  value  = 1ULL;
  result = -EBADF * (write(state & HOST_HANDLE_MASK, &value, sizeof(uint64_t)) <= 0);
  state  = atomic_fetch_sub_explicit(slot, HOST_HANDLE_USER, memory_order_acq_rel) - HOST_HANDLE_USER;

  if ((state & (HOST_HANDLE_GONE | HOST_HANDLE_BUSY)) == HOST_HANDLE_GONE)
  {
    // Peer has gone while the eventfd was written to, and this kick is the last to stop writing to it
    atomic_store_explicit(slot, HOST_HANDLE_EMPTY, memory_order_release);
    close(state & HOST_HANDLE_MASK);
  }

  return result;
}

static void ReleaseHostWormHole(struct WormHole* hole)
{
  int* handle;
  int* limit;

  handle = hole->host.handles;
  limit  = hole->host.handles + UINT16_MAX;

  while (handle <= limit)
  {
    if (*handle != -1)
    {
      // Save syscalls and close only opened eventfd handles
      close(*handle & HOST_HANDLE_MASK);
    }

    handle ++;
  }

  munmap(hole->memory, hole->length);
  close(hole->host.memory);
  close(hole->host.socket);
  close(hole->handle);
  free(hole);
}

static struct WormHole* CreateHostWormHole(const char* path, struct stat* status, HandleWormHoleEventFunction function, void* closure)
{
  struct sockaddr_un address;
  struct WormHole* hole;
  int64_t value;
  int handle;

  if (hole = (struct WormHole*)calloc(1, offsetof(struct WormHole, host) + sizeof(struct HostWormHole)))
  {
    address.sun_family = AF_UNIX;
    hole->host.socket  = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    hole->host.memory  = -1;
    hole->handle       = -1;
    handle             = -1;

    strncpy(address.sun_path, path, sizeof(address.sun_path));
    memset(hole->host.handles, 0xff, sizeof(hole->host.handles));

    if ((hole->host.socket < 0) ||
        (strlen(path) >= sizeof(address.sun_path)) ||
        (connect(hole->host.socket, (struct sockaddr*)&address, sizeof(struct sockaddr_un)) < 0)                 ||
        (ReadHostServerMessage(hole, &value, NULL, 0) < 0)               || (value  != IVSHMEM_PROTOCOL_VERSION) ||
        (ReadHostServerMessage(hole, &hole->identifier, &handle, 0) < 0) || (handle != -1)                       ||
        (ReadHostServerMessage(hole, &value, &hole->host.memory, 0) < 0) || (value  != -1)                       ||
        (fstat(hole->host.memory, status) < 0)                                                                   ||
        ((hole->memory = (uint8_t*)mmap(NULL, status->st_size, PROT_READ | PROT_WRITE, MAP_SHARED, hole->host.memory, 0)) == MAP_FAILED))
    {
      close(hole->host.socket);
      close(hole->host.memory);
      close(handle);
      free(hole);
      return NULL;
    }

    hole->type    = HOLE_CLIENT_TYPE_HOST;
    hole->release = ReleaseHostWormHole;
    hole->kick    = KickHostWormHole;
    hole->event   = function;
    hole->closure = closure;
    hole->length  = status->st_size;
    hole->size    = sizeof(uint64_t);
  }

  return hole;
}

int HandleWormHoleServerEvent(struct WormHole* hole)
{
  _Atomic uint32_t* slot;
  uint32_t state;
  int64_t number;
  int condition;
  int result;
  int handle;

  condition = 0;
  result    = 0;

  while (hole->type == HOLE_CLIENT_TYPE_HOST)
  {
    handle = -1;
    result = ReadHostServerMessage(hole, &number, &handle, MSG_DONTWAIT);

    if (result <= 0)
    {
      if ((result < 0) && (errno == EINTR))
      {
        // Call is interrupted, but data may still be present
        continue;
      }

      if ((result < 0) && (errno == EAGAIN))
      {
        // Socket buffer is empty, that is an normal case
        result = 1;
      }

      break;
    }

    if ((hole->identifier == number) &&
        (hole->handle     == -1))
    {
      // It is allowed to trigger itself to unify behavior between host and guest modes:
      // In case of guest mode it's suitable to communicate between multiple clients on the same host
      hole->handle = dup(handle);
      condition    = (hole->handle >= 0);
    }

    if ((number >= 0) &&
        (number <= UINT16_MAX))
    {
      slot = (_Atomic uint32_t*)hole->host.handles + number;

      if (handle != -1)
      {
        // A peer that has gone under the same ID may still have its eventfd written to, the last write is waited for
        while (((state = atomic_load_explicit(slot, memory_order_acquire)) != HOST_HANDLE_EMPTY) &&
               (state & HOST_HANDLE_GONE));

        if ((state  == HOST_HANDLE_EMPTY) &&
            (handle <= HOST_HANDLE_MASK))
        {
          // Otherwise it is one more vector of a known peer, or a number too large to be kept
          atomic_store_explicit(slot, handle, memory_order_release);
          continue;
        }
      }

      if ((handle == -1) &&
          ((atomic_load_explicit(slot, memory_order_relaxed) & HOST_HANDLE_GONE) == 0))
      {
        if (((state = atomic_fetch_or_explicit(slot, HOST_HANDLE_GONE, memory_order_acq_rel)) & HOST_HANDLE_BUSY) == 0)
        {
          // Otherwise a kick is writing to the eventfd right now, and the last of them closes it
          atomic_store_explicit(slot, HOST_HANDLE_EMPTY, memory_order_release);
          close(state & HOST_HANDLE_MASK);
        }

        continue;
      }
    }

    if (handle != -1)
    {
      close(handle);
      continue;
    }
  }

  if ((condition   != 0) &&
      (hole->event != NULL))
  {
    // Own eventfd is in the table by now, after the ones of the peers that were there before
    hole->event(hole, HOLE_EVENT_READY, hole->identifier);
  }

  if ((hole->type == HOLE_CLIENT_TYPE_HOST) &&
      (result     <= 0))
  {
    // Server has closed the connection or it has failed, there is nothing to expect anymore
    HandleWormHoleLossEvent(hole, HOLE_LOST_SERVER);
  }

  return result;
}

// Guest mode client

#define IVSHMEM_REGISTER_MASK      0
#define IVSHMEM_REGISTER_STATUS    1
#define IVSHMEM_REGISTER_POSITION  2
#define IVSHMEM_REGISTER_DOORBELL  3

#define IVSHMEM_BAR0_SIZE          256

static int ReadSystemValue(struct stat* status, const char* parameter, char* buffer, int size)
{
  char path[PATH_MAX];
  int handle;
  int result;

  snprintf(path, PATH_MAX, "/sys/dev/char/%u:%u/%s", major(status->st_rdev), minor(status->st_rdev), parameter);
  memset(buffer, 0, size);

  result = -1;
  handle = open(path, O_RDONLY);

  if (handle >= 0)
  {
    result = read(handle, buffer, size);
    close(handle);
  }

  return result;
}

static int KickGuestWormHole(struct WormHole* hole, uint32_t identifier)
{
  // Whatever has been written to the region before has to be seen by the peer that hears the bell
  identifier <<= 16;
  atomic_store_explicit((_Atomic uint32_t*)hole->guest.registers + IVSHMEM_REGISTER_DOORBELL, identifier, memory_order_release);
  return 0;
}

static void ReleaseGuestWormHole(struct WormHole* hole)
{
  munmap(hole->guest.registers, IVSHMEM_BAR0_SIZE);
  munmap(hole->memory, hole->length);
  close(hole->handle);
  free(hole);
}

static struct WormHole* CreateGuestWormHole(const char* path, struct stat* status, HandleWormHoleEventFunction function, void* closure)
{
  struct WormHole* hole;
  char buffer[64];
  size_t size;

  if (hole = (struct WormHole*)calloc(1, offsetof(struct WormHole, guest) + sizeof(struct GuestWormHole)))
  {
    size = getpagesize();

    if (((hole->handle          = open(path, O_RDWR | O_SYNC | O_CLOEXEC)) < 0) ||
        (ReadSystemValue(status, "name",           buffer, sizeof(buffer)) < 1) || (strcmp(buffer, "uio_ivshmem\n")                  != 0) ||
        (ReadSystemValue(status, "maps/map1/size", buffer, sizeof(buffer)) < 2) || ((hole->length = strtoumax(buffer + 2, NULL, 16)) == 0) ||
        ((hole->memory          = (uint8_t*)mmap(NULL, hole->length, PROT_READ | PROT_WRITE, MAP_SHARED, hole->handle, size))    == MAP_FAILED) ||
        ((hole->guest.registers = (uint32_t*)mmap(NULL, IVSHMEM_BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, hole->handle, 0)) == MAP_FAILED))
    {
      munmap(hole->guest.registers, IVSHMEM_BAR0_SIZE);
      munmap(hole->memory, hole->length);
      close(hole->handle);
      free(hole);
      return NULL;
    }

    hole->type       = HOLE_CLIENT_TYPE_GUEST;
    hole->release    = ReleaseGuestWormHole;
    hole->kick       = KickGuestWormHole;
    hole->event      = function;
    hole->closure    = closure;
    hole->identifier = hole->guest.registers[IVSHMEM_REGISTER_POSITION];
    hole->size       = sizeof(uint32_t);
  }

  return hole;
}

// Exported interface

struct WormHole* CreateWormHole(const char* path, HandleWormHoleEventFunction function, void* closure)
{
  struct stat status;

  if (stat(path, &status) == 0)
  {
    if (S_ISCHR(status.st_mode))
    {
      // Connection to uio_ivshmem.ko device on the guest system
      return CreateGuestWormHole(path, &status, function, closure);
    }

    if (S_ISSOCK(status.st_mode))
    {
      // Direct connection to ivshmem-server on the host system
      return CreateHostWormHole(path, &status, function, closure);
    }
  }

  return NULL;
}

void HandleWormHoleBellEvent(struct WormHole* hole, int64_t value)
{
  if ((value == HOLE_CLIENT_VALUE_READ) &&
      (read(hole->handle, &value, hole->size) <= 0))
  {
    // Main loop can use blocking I/O or could be (e)poll-based, but a source that has
    // not been granted yet or has nothing to give rings no bell
    return;
  }

  if (hole->event != NULL)
  {
    // Event handler could be unused
    hole->event(hole, HOLE_EVENT_BELL, 0);
  }
}

void HandleWormHoleLossEvent(struct WormHole* hole, int value)
{
  if (hole->event != NULL)
  {
    // Event handler could be unused
    hole->event(hole, HOLE_EVENT_LOST, value);
  }
}
