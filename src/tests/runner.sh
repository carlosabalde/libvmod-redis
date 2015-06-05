#! /bin/bash

##
## Configuration.
##
REDIS_SERVERS=2
REDIS_START_PORT=40000
REDIS_CLUSTER_SERVERS=9
REDIS_CLUSTER_START_PORT=50000
REDIS_CLUSTER_REPLICAS=2
REDIS_CLUSTER_ENABLED=1

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
## Initializations.
##
set -e
TMP=`mktemp -d`
SKIP=1
CONTEXT=""

##
## Register cleanup callback.
##
trap "cleanup $TMP" EXIT

##
## Launch standalone Redis servers?
##
if [[ $2 =~ ^.*standalone[0-9]{2}\.vtc$ ]]; then
    SKIP=0
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
elif [[ $2 =~ ^.*clustered[0-9]{2}\.vtc$ ]] && hash redis-trib.rb 2>/dev/null; then
    SKIP=0
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
        dir $TMP
EOF
        redis-server "$TMP/redis-cserver$INDEX.conf"
        CONTEXT="\
            $CONTEXT \
            -Dredis_cserver${INDEX}_address=127.0.0.1:$((REDIS_CLUSTER_START_PORT+INDEX)) \
            -Dredis_cserver${INDEX}_socket=$TMP/redis-cserver$INDEX.sock"
        CSERVERS="$CSERVERS 127.0.0.1:$((REDIS_CLUSTER_START_PORT+INDEX))"
    done

    yes yes | redis-trib.rb create --replicas $REDIS_CLUSTER_REPLICAS $CSERVERS > /dev/null

    # Wait at least half of NODE_TIMEOUT for all nodes to get the new configuration.
    sleep 3

    # Add to context:
    #   1) All master nodes' addresses ordered by the slots they handle (master1, master2, ...).
    #   2) An example key for each of those master nodes (key_in_master1, key_in_master2, ...).
    INDEX=1
    while read line; do
        CONTEXT="\
            $CONTEXT \
            -Dmaster${INDEX}=$(echo $line | cut -f 2 -d ' ') \
            -Dkey_in_master${INDEX}=$(grep "^$(echo $line | cut -f 9 -d ' ' | cut -f 1 -d '-'): " tests/hashslot_keys.txt | cut -f 2 -d ' ')"
        INDEX=$(( INDEX + 1 ))
    done <<< "$(redis-cli -p $((REDIS_CLUSTER_START_PORT+1)) CLUSTER NODES | grep master | sort -k 9 -n)"
fi

##
## Execute wrapped command?
##
if [[ $SKIP == 0 ]]; then
    set -x
    $@ $CONTEXT
fi
