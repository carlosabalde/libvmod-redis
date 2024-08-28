#! /bin/bash

##
## Configuration.
##
IPV6=0

DB_ENGINE=${DB_ENGINE:-redis}
DB_STANDALONE_MASTER_SERVERS=2
DB_STANDALONE_SLAVE_SERVERS=2
DB_STANDALONE_SENTINEL_SERVERS=3
DB_STANDALONE_START_PORT=40000
DB_CLUSTER_SERVERS=9
DB_CLUSTER_REPLICAS=2
DB_CLUSTER_START_PORT=50000

##
## Cleanup callback.
##
cleanup() {
    set +x

    for MASTER_INDEX in $(seq 1 $DB_STANDALONE_MASTER_SERVERS); do
        if [[ -s "$1/db-master$MASTER_INDEX.pid" ]]; then
            kill -9 $(cat "$1/db-master$MASTER_INDEX.pid")
        fi
        for SLAVE_INDEX in $(seq 1 $DB_STANDALONE_SLAVE_SERVERS); do
            if [[ -s "$1/db-slave${MASTER_INDEX}_$SLAVE_INDEX.pid" ]]; then
                kill -9 $(cat "$1/db-slave${MASTER_INDEX}_$SLAVE_INDEX.pid")
            fi
        done
    done

    for INDEX in $(seq 1 $DB_STANDALONE_SENTINEL_SERVERS); do
        if [[ -s "$1/db-sentinel$INDEX.pid" ]]; then
            kill -9 $(cat "$1/db-sentinel$INDEX.pid")
        fi
    done

    for INDEX in $(seq 1 $DB_CLUSTER_SERVERS); do
        if [[ -s "$1/db-server$INDEX.pid" ]]; then
            kill -9 $(cat "$1/db-server$INDEX.pid")
        fi
    done

    rm -rf "$1"
    echo
}

##
## Initializations.
##
set -e
ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
TMP=`mktemp -d`
chmod o+rwx "$TMP"
CONTEXT=""

##
## Register cleanup callback.
##
trap "cleanup $TMP" EXIT

##
## Check CLI is available & get DB version. Fail test if DB is not available.
##
if [ -x "$(command -v $DB_ENGINE-cli)" ]; then
    VERSION=$($DB_ENGINE-cli --version | sed "s/^$DB_ENGINE-cli \([^ ]*\).*$/\1/" | awk -F. '{ printf("%d%03d%03d\n", $1, $2, $3) }')
    CONTEXT="\
        $CONTEXT \
        -Dredis_version=$VERSION \
        -Dredis_tls_cafile=$ROOT/assets/tls-ca-certificate.crt \
        -Dredis_tls_certfile=$ROOT/assets/tls-certificate.crt \
        -Dredis_tls_keyfile=$ROOT/assets/tls-certificate.key"
else
    echo 'Database not found!'
    exit 1
fi

##
## Silently pass test if minimum DB version is not fulfilled.
##
if [[ ${@: -1} =~ ^.*\.([0-9]{7,})\.[^\.]*\.vtc(\.disabled)?$ ]]; then
    if [ "$VERSION" -lt "${BASH_REMATCH[1]}" ]; then
        echo 'Test silently skipped! A newer DB version is needed.'
        exit 0
    fi
fi

##
## Launch standalone DB servers?
##
if [[ ${@: -1} =~ ^.*standalone(\.[0-9]{7,})?\.[^\.]*\.vtc(\.disabled)?$ ]]; then
    for MASTER_INDEX in $(seq 1 $DB_STANDALONE_MASTER_SERVERS); do
        [[ $IPV6 = 1 ]] && MASTER_IP=::1 || MASTER_IP=127.0.0.$MASTER_INDEX
        MASTER_PORT=$((DB_STANDALONE_START_PORT+MASTER_INDEX))
        MASTER_TLS_PORT=$((MASTER_PORT+1000))
        cat > "$TMP/db-master$MASTER_INDEX.conf" <<EOF
        daemonize yes
        dir $TMP
        bind $MASTER_IP
        port $MASTER_PORT
        unixsocket $TMP/db-master$MASTER_INDEX.sock
        pidfile $TMP/db-master$MASTER_INDEX.pid
EOF
        if [ "$VERSION" -ge '6000000' ]; then
            cat >> "$TMP/db-master$MASTER_INDEX.conf" <<EOF
            tls-port $MASTER_TLS_PORT
            tls-cert-file $ROOT/assets/tls-certificate.crt
            tls-key-file $ROOT/assets/tls-certificate.key
            tls-ca-cert-file $ROOT/assets/tls-ca-certificate.crt
EOF
        fi
        if [ "$VERSION" -ge '7000000' ]; then
            cat >> "$TMP/db-master$MASTER_INDEX.conf" <<EOF
            enable-debug-command local
EOF
        fi
        $DB_ENGINE-server "$TMP/db-master$MASTER_INDEX.conf"
        CONTEXT="\
            $CONTEXT \
            -Dredis_master${MASTER_INDEX}_ip=$MASTER_IP \
            -Dredis_master${MASTER_INDEX}_port=$MASTER_PORT \
            -Dredis_master${MASTER_INDEX}_tls_port=$MASTER_TLS_PORT \
            -Dredis_master${MASTER_INDEX}_socket=$TMP/db-master$MASTER_INDEX.sock"

        for SLAVE_INDEX in $(seq 1 $DB_STANDALONE_SLAVE_SERVERS); do
            [[ $IPV6 = 1 ]] && SLAVE_IP=::1 || SLAVE_IP=127.0.$MASTER_INDEX.$SLAVE_INDEX
            SLAVE_PORT=$((DB_STANDALONE_START_PORT+DB_STANDALONE_MASTER_SERVERS+(MASTER_INDEX-1)*DB_STANDALONE_SLAVE_SERVERS+SLAVE_INDEX))
            SLAVE_TLS_PORT=$((SLAVE_PORT+1000))
            cat > "$TMP/db-slave${MASTER_INDEX}_$SLAVE_INDEX.conf" <<EOF
            daemonize yes
            dir $TMP
            bind $SLAVE_IP
            port $SLAVE_PORT
            unixsocket $TMP/db-slave${MASTER_INDEX}_$SLAVE_INDEX.sock
            pidfile $TMP/db-slave${MASTER_INDEX}_$SLAVE_INDEX.pid
            slaveof $MASTER_IP $MASTER_PORT
EOF
            if [ "$VERSION" -ge '6000000' ]; then
                cat >> "$TMP/db-slave${MASTER_INDEX}_$SLAVE_INDEX.conf" <<EOF
                tls-port $SLAVE_TLS_PORT
                tls-cert-file $ROOT/assets/tls-certificate.crt
                tls-key-file $ROOT/assets/tls-certificate.key
                tls-ca-cert-file $ROOT/assets/tls-ca-certificate.crt
EOF
            fi
            $DB_ENGINE-server "$TMP/db-slave${MASTER_INDEX}_$SLAVE_INDEX.conf"
            CONTEXT="\
                $CONTEXT \
                -Dredis_slave${MASTER_INDEX}_${SLAVE_INDEX}_ip=$SLAVE_IP \
                -Dredis_slave${MASTER_INDEX}_${SLAVE_INDEX}_port=$SLAVE_PORT \
                -Dredis_slave${MASTER_INDEX}_${SLAVE_INDEX}_tls_port=$SLAVE_TLS_PORT \
                -Dredis_slave${MASTER_INDEX}_${SLAVE_INDEX}_socket=$TMP/db-slave${MASTER_INDEX}_$SLAVE_INDEX.sock"
        done

        for SENTINEL_INDEX in $(seq 1 $DB_STANDALONE_SENTINEL_SERVERS); do
            cat >> "$TMP/db-sentinel$SENTINEL_INDEX.conf" <<EOF
            sentinel monitor db-master$MASTER_INDEX $MASTER_IP $MASTER_PORT 1
            sentinel down-after-milliseconds db-master$MASTER_INDEX 5000
            sentinel failover-timeout db-master$MASTER_INDEX 60000
            sentinel parallel-syncs db-master$MASTER_INDEX 1
EOF
        done
    done

    for INDEX in $(seq 1 $DB_STANDALONE_SENTINEL_SERVERS); do
        [[ $IPV6 = 1 ]] && SENTINEL_IP=::1 || SENTINEL_IP=127.1.0.$INDEX
        SENTINEL_PORT=$((DB_STANDALONE_START_PORT+2000+INDEX))
        SENTINEL_TLS_PORT=$((SENTINEL_PORT+1000))
        cat >> "$TMP/db-sentinel$INDEX.conf" <<EOF
        daemonize yes
        dir $TMP
        bind $SENTINEL_IP
        port $SENTINEL_PORT
        pidfile $TMP/db-sentinel$INDEX.pid
        requirepass s3cr3t
EOF
        if [ "$VERSION" -ge '6000000' ]; then
            cat >> "$TMP/db-sentinel$INDEX.conf" <<EOF
            tls-port $SENTINEL_TLS_PORT
            tls-cert-file $ROOT/assets/tls-certificate.crt
            tls-key-file $ROOT/assets/tls-certificate.key
            tls-ca-cert-file $ROOT/assets/tls-ca-certificate.crt
EOF
        fi
        $DB_ENGINE-server "$TMP/db-sentinel$INDEX.conf" --sentinel
        CONTEXT="\
            $CONTEXT \
            -Dredis_sentinel${INDEX}_ip=$SENTINEL_IP \
            -Dredis_sentinel${INDEX}_port=$SENTINEL_PORT \
            -Dredis_sentinel${INDEX}_tls_port=$SENTINEL_TLS_PORT"
    done

##
## Launch clustered DB servers?
##
elif [[ ${@: -1} =~ ^.*clustered(\.[0-9]{7,})?\.[^\.]*\.vtc(\.disabled)?$ ]]; then
    SERVERS=""
    for INDEX in $(seq 1 $DB_CLUSTER_SERVERS); do
        [[ $IPV6 = 1 ]] && IP=::1 || IP=127.0.0.$INDEX
        PORT=$((DB_CLUSTER_START_PORT+INDEX))
        TLS_PORT=$((PORT+1000))
        cat > "$TMP/db-server$INDEX.conf" <<EOF
        daemonize yes
        dir $TMP
        port $PORT
        bind $IP
        unixsocket $TMP/db-server$INDEX.sock
        pidfile $TMP/db-server$INDEX.pid
        cluster-enabled yes
        cluster-announce-ip $IP
        cluster-announce-port $PORT
        cluster-config-file $TMP/db-server$INDEX-nodes.conf
        cluster-node-timeout 5000
        cluster-slave-validity-factor 0
        cluster-require-full-coverage yes
EOF
        if [ "$VERSION" -ge '6000000' ]; then
            cat >> "$TMP/db-server$INDEX.conf" <<EOF
            tls-port $TLS_PORT
            tls-cert-file $ROOT/assets/tls-certificate.crt
            tls-key-file $ROOT/assets/tls-certificate.key
            tls-ca-cert-file $ROOT/assets/tls-ca-certificate.crt
EOF
        fi
        $DB_ENGINE-server "$TMP/db-server$INDEX.conf"
        CONTEXT="\
            $CONTEXT \
            -Dredis_server${INDEX}_ip=$IP \
            -Dredis_server${INDEX}_port=$PORT \
            -Dredis_server${INDEX}_tls_port=$TLS_PORT \
            -Dredis_server${INDEX}_socket=$TMP/db-server$INDEX.sock"
        SERVERS="$SERVERS $IP:$PORT"
    done

    # Wait for all nodes to bootstrap and then set up the cluster.
    sleep 1
    if [ "$VERSION" -ge '5000000' ]; then
        yes yes | $DB_ENGINE-cli --cluster create $SERVERS --cluster-replicas $DB_CLUSTER_REPLICAS > /dev/null
    else
        yes yes | redis-trib.rb create --replicas $DB_CLUSTER_REPLICAS $SERVERS > /dev/null
    fi

    # Wait for cluster formation in a rudementary way.
    [[ $IPV6 = 1 ]] && HOST=::1 || HOST=127.0.0.1
    [[ $IPV6 = 1 ]] && PATTERN=::1 || PATTERN=127[.]0[.]0[.]
    while [ $($DB_ENGINE-cli -h $HOST -p $((DB_CLUSTER_START_PORT+1)) CLUSTER SLOTS | grep "$PATTERN" | wc -l) -lt $DB_CLUSTER_SERVERS ]; do
        sleep 1
    done
    sleep 1

    # Add to context:
    #   - All master nodes' addresses ordered by the slots they handle
    #     (redis_master1, redis_master2, ...).
    #   - An example key for each of those master nodes (redis_key_in_master1,
    #   redis_key_in_master2, ...).
    INDEX=1
    while read LINE; do
        CONTEXT="\
            $CONTEXT \
            -Dredis_master${INDEX}_ip=$(echo $LINE | cut -f 2 -d ' ' | cut -f 1 -d '@' | rev | cut -f 2- -d ':' | rev) \
            -Dredis_master${INDEX}_port=$(echo $LINE | cut -f 2 -d ' ' | cut -f 1 -d '@' | rev | cut -f 1 -d ':' | rev) \
            -Dredis_key_in_master${INDEX}=$(grep "^$(echo $LINE | cut -f 9 -d ' ' | cut -f 1 -d '-'): " $ROOT/assets/hashslot-keys.txt | cut -f 2 -d ' ')"
        INDEX=$(( INDEX + 1 ))
    done <<< "$($DB_ENGINE-cli -h $HOST -p $((DB_CLUSTER_START_PORT+1)) CLUSTER NODES | grep master | sort -k 9 -n)"
fi

##
## Execute wrapped command?
##
set -x
"$1" $CONTEXT "${@:2}"
