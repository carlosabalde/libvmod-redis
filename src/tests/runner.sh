#! /bin/bash

##
## Configuration.
##
REDIS_STANDALONE_MASTER_SERVERS=2
REDIS_STANDALONE_SLAVE_SERVERS=2
REDIS_STANDALONE_START_PORT=40000
REDIS_CLUSTER_SERVERS=9
REDIS_CLUSTER_REPLICAS=2
REDIS_CLUSTER_START_PORT=50000

##
## Cleanup callback.
##
cleanup() {
    set +x

    for MASTER_INDEX in $(seq 1 $REDIS_STANDALONE_MASTER_SERVERS); do
        if [[ -s "$1/redis-master$MASTER_INDEX.pid" ]]; then
            kill -9 $(cat "$1/redis-master$MASTER_INDEX.pid")
        fi
        for SLAVE_INDEX in $(seq 1 $REDIS_STANDALONE_SLAVE_SERVERS); do
            if [[ -s "$1/redis-slave${MASTER_INDEX}_$SLAVE_INDEX.pid" ]]; then
                kill -9 $(cat "$1/redis-slave${MASTER_INDEX}_$SLAVE_INDEX.pid")
            fi
        done
    done

    for INDEX in $(seq 1 $REDIS_CLUSTER_SERVERS); do
        if [[ -s "$1/redis-server$INDEX.pid" ]]; then
            kill -9 $(cat "$1/redis-server$INDEX.pid")
        fi
    done

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
if [[ $2 =~ ^.*standalone\.[^\.]*\.vtc$ ]]; then
    if [ -x "$(command -v redis-cli)" ]; then
        SKIP=0
        for MASTER_INDEX in $(seq 1 $REDIS_STANDALONE_MASTER_SERVERS); do
            MASTER_IP=127.0.0.$MASTER_INDEX
            MASTER_PORT=$((REDIS_STANDALONE_START_PORT+MASTER_INDEX))
            cat > "$TMP/redis-master$MASTER_INDEX.conf" <<EOF
            daemonize yes
            dir $TMP
            bind $MASTER_IP
            port $MASTER_PORT
            unixsocket $TMP/redis-master$MASTER_INDEX.sock
            pidfile $TMP/redis-master$MASTER_INDEX.pid
EOF
            redis-server "$TMP/redis-master$MASTER_INDEX.conf"
            CONTEXT="\
                $CONTEXT \
                -Dredis_master${MASTER_INDEX}_ip=$MASTER_IP \
                -Dredis_master${MASTER_INDEX}_port=$MASTER_PORT \
                -Dredis_master${MASTER_INDEX}_socket=$TMP/redis-master$MASTER_INDEX.sock"
            for SLAVE_INDEX in $(seq 1 $REDIS_STANDALONE_SLAVE_SERVERS); do
                SLAVE_IP=127.0.$MASTER_INDEX.$SLAVE_INDEX
                SLAVE_PORT=$((REDIS_STANDALONE_START_PORT+REDIS_STANDALONE_MASTER_SERVERS+(MASTER_INDEX-1)*REDIS_STANDALONE_SLAVE_SERVERS+SLAVE_INDEX))
                cat > "$TMP/redis-slave${MASTER_INDEX}_$SLAVE_INDEX.conf" <<EOF
                daemonize yes
                dir $TMP
                bind $SLAVE_IP
                port $SLAVE_PORT
                unixsocket $TMP/redis-slave${MASTER_INDEX}_$SLAVE_INDEX.sock
                pidfile $TMP/redis-slave${MASTER_INDEX}_$SLAVE_INDEX.pid
                slaveof $MASTER_IP $MASTER_PORT
EOF
                redis-server "$TMP/redis-slave${MASTER_INDEX}_$SLAVE_INDEX.conf"
                CONTEXT="\
                    $CONTEXT \
                    -Dredis_slave${MASTER_INDEX}_${SLAVE_INDEX}_ip=$SLAVE_IP \
                    -Dredis_slave${MASTER_INDEX}_${SLAVE_INDEX}_port=$SLAVE_PORT \
                    -Dredis_slave${MASTER_INDEX}_${SLAVE_INDEX}_socket=$TMP/redis-slave${MASTER_INDEX}_$SLAVE_INDEX.sock"
            done
        done
    fi

##
## Launch clustered Redis servers?
##
elif [[ $2 =~ ^.*clustered\.[^\.]*\.vtc$ ]]; then
    if [ -x "$(command -v redis-cli)" -a -x "$(command -v redis-trib.rb)" ]; then
        SKIP=0
        SERVERS=""
        for INDEX in $(seq 1 $REDIS_CLUSTER_SERVERS); do
            cat > "$TMP/redis-server$INDEX.conf" <<EOF
            daemonize yes
            dir $TMP
            port $((REDIS_CLUSTER_START_PORT+INDEX))
            bind 127.0.0.$INDEX
            unixsocket $TMP/redis-server$INDEX.sock
            pidfile $TMP/redis-server$INDEX.pid
            cluster-enabled yes
            cluster-config-file $TMP/redis-server$INDEX-nodes.conf
            cluster-node-timeout 5000
            cluster-slave-validity-factor 0
            cluster-require-full-coverage yes
EOF
            redis-server "$TMP/redis-server$INDEX.conf"
            CONTEXT="\
                $CONTEXT \
                -Dredis_server${INDEX}_ip=127.0.0.$INDEX \
                -Dredis_server${INDEX}_port=$((REDIS_CLUSTER_START_PORT+INDEX)) \
                -Dredis_server${INDEX}_socket=$TMP/redis-server$INDEX.sock"
            SERVERS="$SERVERS 127.0.0.$INDEX:$((REDIS_CLUSTER_START_PORT+INDEX))"
        done

        yes yes | redis-trib.rb create --replicas $REDIS_CLUSTER_REPLICAS $SERVERS > /dev/null

        # Wait at least half of NODE_TIMEOUT for all nodes to get the new configuration.
        sleep 3

        # Add to context:
        #   - All master nodes' addresses ordered by the slots they handle
        #     (redis_master1, redis_master2, ...).
        #   - An example key for each of those master nodes (redis_key_in_master1,
        #   redis_key_in_master2, ...).
        INDEX=1
        while read LINE; do
            CONTEXT="\
                $CONTEXT \
                -Dredis_master${INDEX}_ip=$(echo $LINE | cut -f 2 -d ' ' | cut -f 1 -d ':') \
                -Dredis_master${INDEX}_port=$(echo $LINE | cut -f 2 -d ' ' | cut -f 2 -d ':') \
                -Dredis_key_in_master${INDEX}=$(grep "^$(echo $LINE | cut -f 9 -d ' ' | cut -f 1 -d '-'): " tests/hashslot-keys.txt | cut -f 2 -d ' ')"
            INDEX=$(( INDEX + 1 ))
        done <<< "$(redis-cli -p $((REDIS_CLUSTER_START_PORT+1)) CLUSTER NODES | grep master | sort -k 9 -n)"
    fi
fi

##
## Execute wrapped command?
##
if [[ $SKIP == 0 ]]; then
    set -x
    $@ $CONTEXT
fi
