#! /bin/bash

##
## Initializations.
##
set -e
set -x
REDIS1_PORT=50001
REDIS2_PORT=50002
TMP=`mktemp -d`

##
## Cleanup callback.
##
cleanup() {
    kill -9 $(cat "$1/redis1.pid")
    kill -9 $(cat "$1/redis2.pid")
    rm -rf "$1"
}

##
## Temporarily launch a couple of Redis servers.
##
cat > "$TMP/redis1.conf" <<EOF
daemonize yes
port $REDIS1_PORT
bind 127.0.0.1
pidfile $TMP/redis1.pid
EOF
redis-server "$TMP/redis1.conf"

cat > "$TMP/redis2.conf" <<EOF
daemonize yes
port $REDIS2_PORT
bind 127.0.0.1
pidfile $TMP/redis2.pid
EOF
redis-server "$TMP/redis2.conf"

##
## Register cleanup callback.
##
trap "cleanup $TMP" EXIT

##
## Execute wrapped command.
##
$@ -Dredis1_port=$REDIS1_PORT \
   -Dredis2_port=$REDIS2_PORT
