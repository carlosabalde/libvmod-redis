
.. image:: https://travis-ci.org/carlosabalde/libvmod-redis.svg?branch=4.1
   :alt: Travis CI badge
   :target: https://travis-ci.org/carlosabalde/libvmod-redis/

VMOD using the `synchronous hiredis library API <https://github.com/redis/hiredis>`_ to access Redis servers from VCL.

Highlights:

* **Full support for execution of LUA scripts** (i.e. ``EVAL`` command), including optimistic automatic execution of ``EVALSHA`` commands.
* **All Redis reply data types are supported**, including partial support to access to components of simple (i.e. not nested) array replies.
* **Redis pipelines are not (and won't be) supported**. LUA scripting, which is fully supported by the VMOD, it's a much more flexible alternative to pipelines for atomic execution and minimizing latency. Pipelines are hard to use and error prone, specially when using the ``WATCH`` command.
* **Support for classic Redis deployments** using multiple replicated Redis servers **and for clustered deployments based on Redis Cluster**.
* **Support for multiple databases and multiple Redis connections**, local to each Varnish worker thread, or shared using one or more pools.
* **Support for smart command execution**, selecting the destination server according with the preferred role (i.e. master or slave) and with distance and healthiness metrics collected during execution.
* **Support for Redis Sentinel**, allowing automatic discovery of sick / healthy servers and changes in their roles.

Please, check out `the project wiki <https://github.com/carlosabalde/libvmod-redis/wiki>`_ for some extra information and useful links.

Looking for official support for this VMOD? Please, contact `Allenta Consulting <https://www.allenta.com>`_, a `Varnish Software Premium partner <https://www.varnish-software.com/partner/allenta-consulting>`_.

SYNOPSIS
========

import redis;

::

    # Subnets.
    Function subnets(STRING masks="")

    # Sentinels.
    Function sentinels(
        STRING locations="",
        INT period=60,
        INT connection_timeout=500,
        INT command_timeout=0)

    # Databases.
    Object db(
        STRING location,
        ENUM { master, slave, auto, cluster } type,
        INT connection_timeout=1000,
        INT connection_ttl=0,
        INT command_timeout=0,
        INT max_command_retries=0,
        BOOL shared_connections=true,
        INT max_connections=128,
        STRING password="",
        INT sickness_ttl=60,
        INT max_cluster_hops=32)
    Method VOID .add_server(
        STRING location,
        ENUM { master, slave, auto, cluster } type)

    # Command execution.
    Method VOID .command(STRING name)
    Method VOID .timeout(INT command_timeout)
    Method VOID .retries(INT max_command_retries)
    Method VOID .push(STRING arg)
    Method VOID .execute(BOOL master=true)

    # Access to replies.
    Method BOOL .replied()

    Method BOOL .reply_is_error()
    Method BOOL .reply_is_nil()
    Method BOOL .reply_is_status()
    Method BOOL .reply_is_integer()
    Method BOOL .reply_is_string()
    Method BOOL .reply_is_array()

    Method STRING .get_reply()

    Method STRING .get_error_reply()
    Method STRING .get_status_reply()
    Method INT .get_integer_reply()
    Method STRING .get_string_reply()

    Method INT .get_array_reply_length()
    Method BOOL .array_reply_is_error(INT index)
    Method BOOL .array_reply_is_nil(INT index)
    Method BOOL .array_reply_is_status(INT index)
    Method BOOL .array_reply_is_integer(INT index)
    Method BOOL .array_reply_is_string(INT index)
    Method BOOL .array_reply_is_array(INT index)
    Method STRING .get_array_reply_value(INT index)

    # Other.
    Method VOID .free()
    Method STRING .stats()
    Method INT .counter(STRING name)

EXAMPLES
========

Single server
-------------

::

    sub vcl_init {
        # VMOD configuration: simple case, keeping up to one Redis connection
        # per Varnish worker thread.
        new db = redis.db(
            location="192.168.1.100:6379",
            connection_timeout=500,
            shared_connections=false,
            max_connections=1,
            clustered=false);
    }

    sub vcl_deliver {
        # Simple command execution.
        db.command("SET");
        db.push("foo");
        db.push("Hello world!");
        db.execute();

        # LUA scripting.
        db.command("EVAL");
        db.push({"
            redis.call('SET', KEYS[1], ARGV[1])
            redis.call('SET', KEYS[2], ARGV[1])
        "});
        db.push("2");
        db.push("foo");
        db.push("bar");
        db.push("Atomic hello world!");
        db.execute();

        # Array replies, checking & accessing to reply.
        db.command("MGET");
        db.push("foo");
        db.push("bar");
        db.execute();
        if ((db.reply_is_array()) &&
            (db.get_array_reply_length() == 2)) {
            set resp.http.X-Foo = db.get_array_reply_value(0);
            set resp.http.X-Bar = db.get_array_reply_value(1);
        }
    }

Multiple servers
----------------

::

    sub vcl_init {
        # VMOD configuration: master-slave replication, keeping up to two
        # Redis connections per Varnish worker thread (up to one to the master
        # server & up to one to the closest slave server).
        redis.subnets(
            masks={"
                0 192.168.1.102/32,
                1 192.168.1.103/32,
                2 0.0.0.0/32
            "});
        new db = redis.db(
            location="192.168.1.100:6379",
            type=master,
            connection_timeout=500,
            shared_connections=false,
            max_connections=2
            clustered=false);
        db.add_server("192.168.1.101:6379", slave);
        db.add_server("192.168.1.102:6379", slave);
        db.add_server("192.168.1.103:6379", slave);
    }

    sub vcl_deliver {
        # SET submitted to the master server.
        db.command("SET");
        db.push("foo");
        db.push("Hello world!");
        db.execute();

        # GET submitted to one of the slave servers.
        db.command("GET");
        db.push("foo");
        db.execute(false);
        set req.http.X-Foo = db.get_string_reply();
    }

Clustered setup
---------------

::

    sub vcl_init {
        # VMOD configuration: clustered setup, keeping up to 100 Redis
        # connections per server, all shared between all Varnish worker threads.
        # Two initial cluster servers are provided; remaining servers are
        # automatically discovered.
        new db = redis.db(
            location="192.168.1.100:6379",
            type=cluster,
            connection_timeout=500,
            shared_connections=true,
            max_connections=128,
            max_cluster_hops=16);
        cluster.add_server("192.168.1.101:6379", cluster);
    }

    sub vcl_deliver {
        # SET internally routed to the destination server.
        db.command("SET");
        db.push("foo");
        db.push("Hello world!");
        db.execute();

        # GET internally routed to the destination server.
        db.command("GET");
        db.push("foo");
        db.execute(false);
        set req.http.X-Foo = db.get_string_reply();
    }

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

Dependencies:

* `hiredis <https://github.com/redis/hiredis>`_ - minimalistic C Redis client library.
* `libev <http://software.schmorp.de/pkg/libev.html>`_ - full-featured and high-performance event loop.

COPYRIGHT
=========

See LICENSE for details.

Implementation of the SHA-1 and CRC-16 cryptographic hash functions embedded in this VMOD (required to the optimistic execution of ``EVALSHA`` commands, and to the Redis Cluster slot calculation, respectively) are borrowed from the Redis implementation:

* http://download.redis.io/redis-stable/src/sha1.c
* http://download.redis.io/redis-stable/src/sha1.h
* http://download.redis.io/redis-stable/src/crc16.c
* http://download.redis.io/redis-stable/src/config.h
* http://download.redis.io/redis-stable/src/solarisfixes.h

Copyright (c) 2014-2016 Carlos Abalde <carlos.abalde@gmail.com>
