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

#include <stdio.h>

// Host mode client

#define IVSHMEM_PROTOCOL_VERSION  0

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
  uint64_t value;

  value = 1ULL;

  return -EBADF *
    ((hole->host.handles[identifier] < 0) ||
     (write(hole->host.handles[identifier], &value, sizeof(uint64_t)) <= 0));
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
      close(*handle);
    }

    handle ++;
  }

  munmap(hole->memory, hole->length);
  close(hole->host.memory);
  close(hole->host.socket);
  close(hole->handle);
  free(hole);
}

static struct WormHole* CreateHostWormHole(const char* path, struct stat* status, HandleWormHoleBellFunction function, void* closure)
{
  struct sockaddr_un address;
  struct WormHole* hole;
  int64_t value;
  int handle;

  hole = (struct WormHole*)calloc(1, offsetof(struct WormHole, host) + sizeof(struct HostWormHole));

  address.sun_family = AF_UNIX;
  hole->host.socket  = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  hole->host.memory  = -1;
  hole->handle       = -1;
  handle             = -1;

  strncpy(address.sun_path, path, sizeof(address.sun_path));
  memset(hole->host.handles, 0xff, sizeof(hole->host.handles));

  if ((hole->host.socket < 0) ||
      (connect(hole->host.socket, (struct sockaddr*)&address, sizeof(struct sockaddr_un)) < 0)                 ||
      (ReadHostServerMessage(hole, &value, NULL, 0) < 0)               || (value  != IVSHMEM_PROTOCOL_VERSION) ||
      (ReadHostServerMessage(hole, &hole->identifier, &handle, 0) < 0) || (handle != -1)                       ||
      (ReadHostServerMessage(hole, &value, &hole->host.memory, 0) < 0) || (value  != -1)                       ||
      (fstat(hole->host.memory, status) < 0)                                                                   ||
      ((hole->memory = (uint8_t*)mmap(NULL, status->st_size, PROT_READ | PROT_WRITE, MAP_SHARED, hole->host.memory, 0)) == MAP_FAILED))
  {
    close(hole->host.socket);
    close(hole->host.memory);
    free(hole);
    return NULL;
  }

  hole->type    = HOLE_CLIENT_TYPE_HOST;
  hole->release = ReleaseHostWormHole;
  hole->kick    = KickHostWormHole;
  hole->bell    = function;
  hole->closure = closure;
  hole->length  = status->st_size;
  hole->size    = sizeof(uint64_t);

  return hole;
}

int HandleWormHoleHostServerEvent(struct WormHole* hole)
{
  int64_t number;
  int handle;
  int result;

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
        return 1;
      }

      return result;
    }

    if ((hole->identifier == number) &&
        (hole->handle     == -1))
    {
      // It is allowed to trigger itself to unify behavior between host and guest modes:
      // In case of guest mode it's suitable to communicate between multiple clients on the same host
      hole->handle = dup(handle);
    }

    if ((number >= 0) &&
        (number <= UINT16_MAX))
    {
      if ((handle != -1) && (hole->host.handles[number] == -1))
      {
        hole->host.handles[number] = handle;
        continue;
      }

      if ((handle == -1) && (hole->host.handles[number] != -1))
      {
        close(hole->host.handles[number]);
        hole->host.handles[number] = -1;
        continue;
      }
    }

    if (handle != -1)
    {
      close(handle);
      continue;
    }
  }

  return 0;
}

// Guest mode client

#define IVSHMEM_REGISTER_MASK      0
#define IVSHMEM_REGISTER_STATUS    1
#define IVSHMEM_REGISTER_POSITION  2
#define IVSHMEM_REGISTER_DOORBELL  3

#define IVSHMEM_BAR0_SIZE          256

static int ReadSystemValue(int handle, const char* kind, const char* parameter, char* buffer, int size)
{
  char temporary1[PATH_MAX];
  char temporary2[PATH_MAX];
  char* name;
  int result;

  // /proc/self/fd/NNN -> /dev/uio0 -> /sys/class/uio/uio0/maps/map1/size

  snprintf(temporary1, PATH_MAX, "/proc/self/fd/%d", handle);
  memset(buffer, 0, size);

  result = -1;
  handle = -1;

  if ((readlink(temporary1, temporary2, PATH_MAX) > 0) &&
      (name = strrchr(temporary2, '/')) &&
      (snprintf(temporary1, PATH_MAX, "/sys/%s%s/%s", kind, name, parameter) > 0) &&
      ((handle = open(temporary1, O_RDONLY)) >= 0))
  {
    result = read(handle, buffer, size);
    close(handle);
  }

  return result;
}

static int KickGuestWormHole(struct WormHole* hole, uint32_t identifier)
{
  identifier <<= 16;
  atomic_store_explicit((_Atomic uint32_t*)hole->guest.registers + IVSHMEM_REGISTER_DOORBELL, identifier, memory_order_relaxed);
  return 0;
}

static void ReleaseGuestWormHole(struct WormHole* hole)
{
  munmap(hole->guest.registers, IVSHMEM_BAR0_SIZE);
  munmap(hole->memory, hole->length);
  close(hole->handle);
  free(hole);
}

static struct WormHole* CreateGuestWormHole(const char* path, struct stat* status, HandleWormHoleBellFunction function, void* closure)
{
  struct WormHole* hole;
  char buffer[64];
  size_t size;

  size = getpagesize();
  hole = (struct WormHole*)calloc(1, offsetof(struct WormHole, guest) + sizeof(struct GuestWormHole));

  if (((hole->handle          = open(path, O_RDWR | O_SYNC | O_CLOEXEC)) < 0) ||
      (ReadSystemValue(hole->handle, "class/uio", "name",           buffer, sizeof(buffer)) < 1) || (strcmp(buffer, "uio_ivshmem\n")                  != 0) ||
      (ReadSystemValue(hole->handle, "class/uio", "maps/map1/size", buffer, sizeof(buffer)) < 2) || ((hole->length = strtoumax(buffer + 2, NULL, 16)) == 0) ||
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
  hole->bell       = function;
  hole->closure    = closure;
  hole->identifier = hole->guest.registers[IVSHMEM_REGISTER_POSITION];
  hole->size       = sizeof(uint32_t);

  return hole;
}

// Exported interface

struct WormHole* CreateWormHole(const char* path, HandleWormHoleBellFunction function, void* closure)
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

void HandleWormHoleInterruptEvent(struct WormHole* hole, int64_t value)
{
  if (value == HOLE_CLIENT_VALUE_READ)
  {
    // Main loop can use blocking I/O or could be (e)poll-based
    read(hole->handle, &value, hole->size);
  }

  if (hole->bell != NULL)
  {
    // Bell could be unused
    hole->bell(hole);
  }
}
