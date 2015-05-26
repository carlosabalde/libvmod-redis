#! /bin/bash

##
## Initializations.
##
set -e
set -x
REDIS_SERVERS=2
REDIS_START_PORT=40000
REDIS_CLUSTER_SERVERS=9
REDIS_CLUSTER_START_PORT=50000
REDIS_CLUSTER_REPLICAS=2
VTC_DEFINES=""
TMP=`mktemp -d`

##
## Cleanup callback.
##
cleanup() {
    for INDEX in `seq 1 $REDIS_SERVERS`; do
        kill -9 $(cat "$1/redis-master$INDEX.pid")
    done
    rm -rf "$1"
}

##
## Launch standalone Redis servers.
##
for INDEX in `seq 1 $REDIS_SERVERS`; do
    cat > "$TMP/redis-master$INDEX.conf" <<EOF
daemonize yes
port $((REDIS_START_PORT+INDEX))
bind 127.0.0.1
unixsocket $TMP/redis-master$INDEX.sock
pidfile $TMP/redis-master$INDEX.pid
EOF
    redis-server "$TMP/redis-master$INDEX.conf"
    VTC_DEFINES="\
        $VTC_DEFINES \
        -Dredis_master${INDEX}_address=127.0.0.1:$((REDIS_START_PORT+INDEX)) \
        -Dredis_master${INDEX}_socket=$TMP/redis-master$INDEX.sock"
done

##
## Launch clustered Redis servers.
##

##
## Register cleanup callback.
##
trap "cleanup $TMP" EXIT

##
## Execute wrapped command.
##
$@ \
    -Dredis_servers=$REDIS_SERVERS \
    -Dredis_start_port=$REDIS_START_PORT \
    -Dredis_cluster_servers=$REDIS_CLUSTER_SERVERS \
    -Dredis_cluster_start_port=$REDIS_CLUSTER_START_PORT \
    -Dredis_cluster_replicas=$REDIS_CLUSTER_REPLICAS \
    $VTC_DEFINES
