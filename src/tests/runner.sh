#! /bin/bash

##
## Initializations.
##
set -e
REDIS_SERVERS=2
REDIS_START_PORT=40000
REDIS_CLUSTER_SERVERS=9
REDIS_CLUSTER_START_PORT=50000
REDIS_CLUSTER_REPLICAS=2
REDIS_CLUSTER_ENABLED=0
CONTEXT=""
TMP=`mktemp -d`

##
## Cleanup callback.
##
cleanup() {
    set +x

    for INDEX in $(seq 1 $REDIS_SERVERS); do
        if [[ -s "$1/redis-master$INDEX.pid" ]]; then
            kill -9 $(cat "$1/redis-master$INDEX.pid")
        fi
    done

    if [[ $REDIS_CLUSTER_ENABLED == 1 ]]; then
        for INDEX in $(seq 1 $REDIS_CLUSTER_SERVERS); do
            if [[ -s "$1/redis-cserver$INDEX.pid" ]]; then
                kill -9 $(cat "$1/redis-cserver$INDEX.pid")
            fi
        done
    fi

    rm -rf "$1"
    echo
}

##
## Register cleanup callback.
##
trap "cleanup $TMP" EXIT

##
## Launch standalone Redis servers.
##
for INDEX in $(seq 1 $REDIS_SERVERS); do
    cat > "$TMP/redis-master$INDEX.conf" <<EOF
daemonize yes
port $((REDIS_START_PORT+INDEX))
bind 127.0.0.1
unixsocket $TMP/redis-master$INDEX.sock
pidfile $TMP/redis-master$INDEX.pid
EOF
    redis-server "$TMP/redis-master$INDEX.conf"
    CONTEXT="\
        $CONTEXT \
        -Dredis_master${INDEX}_address=127.0.0.1:$((REDIS_START_PORT+INDEX)) \
        -Dredis_master${INDEX}_socket=$TMP/redis-master$INDEX.sock"
done

##
## Launch clustered Redis servers?
##
if [[ $2 =~ ^.*clustered[0-9]{2}\.vtc$ ]] && hash redis-trib.rb 2>/dev/null; then
    REDIS_CLUSTER_ENABLED=1
    CSERVERS=""
    for INDEX in $(seq 1 $REDIS_CLUSTER_SERVERS); do
        cat > "$TMP/redis-cserver$INDEX.conf" <<EOF
        daemonize yes
        port $((REDIS_CLUSTER_START_PORT+INDEX))
        bind 127.0.0.1
        unixsocket $TMP/redis-cserver$INDEX.sock
        pidfile $TMP/redis-cserver$INDEX.pid
        cluster-enabled yes
        cluster-config-file $TMP/redis-cserver$INDEX-nodes.conf
        cluster-node-timeout 5000
        cluster-slave-validity-factor 0
        cluster-require-full-coverage yes
EOF
        redis-server "$TMP/redis-cserver$INDEX.conf"
        CONTEXT="\
            $CONTEXT \
            -Dredis_cserver${INDEX}_address=127.0.0.1:$((REDIS_CLUSTER_START_PORT+INDEX)) \
            -Dredis_cserver${INDEX}_socket=$TMP/redis-cserver$INDEX.sock"
        CSERVERS="$CSERVERS 127.0.0.1:$((REDIS_CLUSTER_START_PORT+INDEX))"
    done
    yes yes | redis-trib.rb create --replicas $REDIS_CLUSTER_REPLICAS $CSERVERS > /dev/null
fi

##
## Execute wrapped command.
##
set -x
$@ $CONTEXT
