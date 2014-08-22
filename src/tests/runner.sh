#! /bin/bash

##
## Initializations.
##
set -e
set -x
REDIS_PORT=50000
TMP=`mktemp -d`

##
## Cleanup callback.
##
cleanup() {
    kill -9 $(cat "$1/redis.pid")
    rm -rf "$1"
}

##
## Temporarily launch a local Redis server.
##
cat > "$TMP/redis.conf" <<EOF
daemonize yes
port $REDIS_PORT
bind 127.0.0.1
pidfile $TMP/redis.pid
EOF
redis-server "$TMP/redis.conf"

##
## Register cleanup callback.
##
trap "cleanup $TMP" EXIT

##
## Execute wrapped command.
##
$@ -Dredis_port=$REDIS_PORT
