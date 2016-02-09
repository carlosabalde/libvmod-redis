
.. image:: https://travis-ci.org/carlosabalde/libvmod-redis.svg?branch=4.1
   :alt: Travis CI badge
   :target: https://travis-ci.org/carlosabalde/libvmod-redis/

VMOD using the `synchronous hiredis library API <https://github.com/redis/hiredis>`_ to access Redis servers from VCL.

Highlights:

* **Full support for execution of LUA scripts** (i.e. ``EVAL`` command), including optimistic automatic execution of ``EVALSHA`` commands.
* **All Redis reply data types are supported**, including partial support to access to components of simple (i.e. not nested) array replies.
* **Redis pipelines are not (and won't be) supported**. LUA scripting, which is fully supported by the VMOD, it's a much more flexible alternative to pipelines for atomic execution and minimizing latency. Pipelines are hard to use and error prone, specially when using the ``WATCH`` command.
* **Support for classic Redis deployments** using multiple Redis servers (replicated or standalone) **and for clustered deployments based on Redis Cluster**.
* **Support for multiple Redis connections**, local to each Varnish worker thread, or shared using one or more pools.

Please, check out `the project wiki <https://github.com/carlosabalde/libvmod-redis/wiki>`_ for some extra information and useful links.

Looking for official support for this VMOD? Please, contact `Allenta Consulting <https://www.allenta.com>`_, a `Varnish Software Premium partner <https://www.varnish-software.com/partner/allenta-consulting>`_.

SYNOPSIS
========

import redis;

::

    # Configuration.
    Object db(
        LOCATION, CONNECTION_TIMEOUT, CONNECTION_TTL, COMMAND_TIMEOUT, COMMAND_RETRIES,
        SHARED_CONTEXTS, MAX_CONTEXTS, PASSWORD, CLUSTERED, MAX_CLUSTER_HOPS)
    Method VOID .add_server(LOCATION)

    # Command execution.
    Method VOID .command(COMMAND)
    Method VOID .timeout(COMMAND_TIMEOUT)
    Method VOID .retries(COMMAND_RETRIES)
    Method VOID .push(ARGUMENT)
    Method VOID .execute()

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
    Method BOOL .array_reply_is_error(INDEX)
    Method BOOL .array_reply_is_nil(INDEX)
    Method BOOL .array_reply_is_status(INDEX)
    Method BOOL .array_reply_is_integer(INDEX)
    Method BOOL .array_reply_is_string(INDEX)
    Method BOOL .array_reply_is_array(INDEX)
    Method STRING .get_array_reply_value(INDEX)

    # Other.
    Method VOID .free()
    Method STRING .stats()
    Method INT .counter(NAME)

EXAMPLES
========

Single server
-------------

::

    sub vcl_init {
        # VMOD configuration: simple case, keeping up to one Redis connection
        # per Varnish worker thread.
        new db = redis.db("192.168.1.100:6379", 500, 0, 0, 0, false, 1, "", false, 0);
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
            redis.call('SET', KEYS[1], ARGV[1]);
            redis.call('SET', KEYS[2], ARGV[1]);
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
        # server & up to one to a randomly selected slave server).
        new master = redis.db("192.168.1.100:6379", 500, 0, 0, 0, false, 1, "", false, 0);
        new slave = redis.db("192.168.1.101:6379", 500, 0, 0, 0, false, 1, "", false, 0);
        slave.add_server("192.168.1.102:6379");
        slave.add_server("192.168.1.103:6379");
    }

    sub vcl_deliver {
        # SET submitted to the master server.
        master.command("SET");
        master.push("foo");
        master.push("Hello world!");
        master.execute();

        # GET submitted to one of the slave servers.
        slave.command("GET");
        slave.push("foo");
        slave.execute();
        set req.http.X-Foo = slave.get_string_reply();
    }

Clustered setup
---------------

::

    sub vcl_init {
        # VMOD configuration: clustered setup, keeping up to 100 Redis
        # connections per server, all shared between all Varnish worker threads.
        # Two initial cluster servers are provided; remaining servers are
        # automatically discovered.
        new cluster = redis.db("192.168.1.100:6379", 500, 0, 0, 0, true, 100, "", true, 16);
        cluster.add_server("192.168.1.101:6379");
    }

    sub vcl_deliver {
        # SET internally routed to the destination server.
        cluster.command("SET");
        cluster.push("foo");
        cluster.push("Hello world!");
        cluster.execute();

        # GET internally routed to the destination server.
        cluster.command("GET");
        cluster.push("foo");
        cluster.execute();
        set req.http.X-Foo = cluster.get_string_reply();
    }

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

Dependencies:

* `hiredis <https://github.com/redis/hiredis>`_ - minimalistic C Redis client library.

COPYRIGHT
=========

See LICENSE for details.

Implementation of the SHA-1 and CRC-16 cryptographic hash functions embedded in this VMOD (required to the optimistic execution of ``EVALSHA`` commands, and to the Redis Cluster slot calculation, respectively) are borrowed from the Redis implementation:

* http://download.redis.io/redis-stable/src/sha1.c
* http://download.redis.io/redis-stable/src/sha1.h
* http://download.redis.io/redis-stable/src/crc16.c
* http://download.redis.io/redis-stable/src/config.h
* http://download.redis.io/redis-stable/src/solarisfixes.h

Copyright (c) 2014-2015 Carlos Abalde <carlos.abalde@gmail.com>
