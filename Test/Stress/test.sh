#!/bin/bash
#
# test.sh [seconds] [path to the socket of a running ivshmem-server]
#
# Without a socket, the stand-in server.py is started on one of its own. Every kick is
# delayed by 50 us between taking the number of an eventfd and writing to it, so that a
# race between a kick and a peer that leaves shows up many times a run rather than now and then

SECONDS_TO_RUN=${1:-30}
SOCKET=$2

make || exit 1

if [ -z "$SOCKET" ]
then
  SOCKET=/tmp/wormhole-stress.sock
  python3 ./server.py $SOCKET &
  SERVER=$!
  sleep 1
fi

python3 ./churn.py $SOCKET $SECONDS_TO_RUN &
CHURN1=$!
python3 ./churn.py $SOCKET $SECONDS_TO_RUN &
CHURN2=$!

./stress $SOCKET $SECONDS_TO_RUN
RESULT=$?

wait $CHURN1 $CHURN2

if [ -n "$SERVER" ]
then
  kill $SERVER
  rm -f $SOCKET
fi

exit $RESULT
