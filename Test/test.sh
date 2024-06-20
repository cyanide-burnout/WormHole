#!/bin/bash

SERVER=../../qemu/build/contrib/ivshmem-server/ivshmem-server

# mkdir qemu/build && cd qemu/build
# ../configure && make

if [ "$1" == "1" ]
then
  make

  $SERVER -F -v -l 2048000 -n 1 &
  PROCESS1=$$

  ./test $SOCKET &
  PROCESS2=$$

  sleep 1
  ./test /tmp/ivshmem_socket

  kill $PROCESS2
  kill $PROCESS1
fi

if [ "$1" == "2" ]
then
  make

  $SERVER -F -v -l 2048000 -n 1 &
  PROCESS1=$$

  ./test /tmp/ivshmem_socket &
  PROCESS2=$$

  virt-builder debian-12 \
    --arch x86_64 \
    --output /tmp/test.img \
    --root-password password:test \
    --mkdir /opt/WormHole \
    --copy-in ../:/opt/WormHole/ \
    --copy-in ../../FastRing/Ring:/opt/WormHole/ \
    --firstboot-command '/opt/WormHole/guest.sh' \
  && \
  sudo qemu-system-x86_64 \
    -m 4096M \
    -nographic \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    -drive id=disk0,file=/tmp/test.img,if=none \
    -device virtio-net,netdev=net0 \
    -netdev tap,id=net0,script=./bridge.sh \
    -device ivshmem-doorbell,vectors=1,chardev=ivshmem0 \
    -chardev socket,path=/tmp/ivshmem_socket,id=ivshmem0

  kill $PROCESS2
  kill $PROCESS1
fi
