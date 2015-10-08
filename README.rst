
.. image:: https://travis-ci.org/carlosabalde/libvmod-redis.svg?branch=4.0
   :alt: Travis CI badge
   :target: https://travis-ci.org/carlosabalde/libvmod-redis/

VMOD using the synchronous hiredis library API (https://github.com/redis/hiredis) to access Redis servers from VCL.

Highlights:

* **Full support for execution of LUA scripts** (i.e. ``EVAL`` command), including optimistic automatic execution of ``EVALSHA`` commands.
* **All Redis reply data types are supported**, including partial support to access to components of simple (i.e. not nested) array replies.
* **Redis pipelines are not (and won't be) supported**. LUA scripting, which is fully supported by the VMOD, it's a much more flexible alternative to pipelines for atomic execution and minimizing latency. Pipelines are hard to use and error prone, specially when using the ``WATCH`` command.
* **Support for classic Redis deployments** using multiple Redis servers (replicated or standalone) **and for clustered deployments based on Redis Cluster**.
* **Support for multiple Redis connections**, local to each Varnish worker thread, or shared using one or more pools.

Please, check out the project wiki at https://github.com/carlosabalde/libvmod-redis/wiki for some extra information and useful links.

Looking for official support for this VMOD? Please, contact Allenta Consulting (http://www.allenta.com), the Varnish Software integration partner for Spain and Portugal (https://www.varnish-software.com/partner/allenta-consulting).

SYNOPSIS
========

import redis;

::

    # Configuration.
    Function VOID init(TAG, LOCATION, CONNECTION_TIMEOUT, CONNECTION_TTL, COMMAND_TIMEOUT, MAX_CLUSTER_HOPS, RETRIES, SHARED_CONTEXTS, MAX_CONTEXTS)
    Function VOID add_server(TAG, LOCATION, CONNECTION_TIMEOUT, CONNECTION_TTL)
    Function VOID add_cserver(LOCATION)

    # Command execution.
    Function VOID command(COMMAND)
    Function VOID server(TAG)
    Function VOID timeout(COMMAND_TIMEOUT)
    Function VOID push(ARGUMENT)
    Function VOID execute()

    # Access to replies.
    Function BOOL replied()

    Function BOOL reply_is_error()
    Function BOOL reply_is_nil()
    Function BOOL reply_is_status()
    Function BOOL reply_is_integer()
    Function BOOL reply_is_string()
    Function BOOL reply_is_array()

    Function STRING get_reply()

    Function STRING get_error_reply()
    Function STRING get_status_reply()
    Function INT get_integer_reply()
    Function STRING get_string_reply()

    Function INT get_array_reply_length()
    Function BOOL array_reply_is_error(INDEX)
    Function BOOL array_reply_is_nil(INDEX)
    Function BOOL array_reply_is_status(INDEX)
    Function BOOL array_reply_is_integer(INDEX)
    Function BOOL array_reply_is_string(INDEX)
    Function BOOL array_reply_is_array(INDEX)
    Function STRING get_array_reply_value(INDEX)

    # Other.
    Function VOID free()
    Function VOID fini()

EXAMPLES
========

Single server
-------------

::

    sub vcl_init {
        # VMOD configuration: simple case, keeping up to one Redis connection
        # per Varnish worker thread.
        redis.init("main", "192.168.1.100:6379", 500, 0, 0, 0, 0, false, 1);
    }

    sub vcl_deliver {
        # Simple command execution.
        redis.command("SET");
        redis.push("foo");
        redis.push("Hello world!");
        redis.execute();

        # LUA scripting.
        redis.command("EVAL");
        redis.push({"
            redis.call('SET', KEYS[1], ARGV[1]);
            redis.call('SET', KEYS[2], ARGV[1]);
        "});
        redis.push("2");
        redis.push("foo");
        redis.push("bar");
        redis.push("Atomic hello world!");
        redis.execute();

        # Array replies, checking & accessing to reply.
        redis.command("MGET");
        redis.push("foo");
        redis.push("bar");
        redis.execute();
        if ((redis.reply_is_array()) &&
            (redis.get_array_reply_length() == 2)) {
            set resp.http.X-Foo = redis.get_array_reply_value(0);
            set resp.http.X-Bar = redis.get_array_reply_value(1);
        }
    }

Multiple servers
----------------

::

    sub vcl_init {
        # VMOD configuration: master-slave replication, keeping up to two
        # Redis connections per Varnish worker thread (up to one to the master
        # server & up to one to a randomly selected slave server).
        redis.init("master", "192.168.1.100:6379", 500, 0, 0, 0, 0, false, 2);
        redis.add_server("slave", "192.168.1.101:6379", 500, 0);
        redis.add_server("slave", "192.168.1.102:6379", 500, 0);
        redis.add_server("slave", "192.168.1.103:6379", 500, 0);
    }

    sub vcl_deliver {
        # SET submitted to the master server.
        redis.command("SET");
        redis.server("master");
        redis.push("foo");
        redis.push("Hello world!");
        redis.execute();

        # GET submitted to one of the slave servers.
        redis.command("GET");
        redis.server("slave");
        redis.push("foo");
        redis.execute();
        set req.http.X-Foo = redis.get_string_reply();
    }

Clustered setup
---------------

::

    sub vcl_init {
        # VMOD configuration: clustered setup, keeping up to 100 Redis
        # connections per server, all shared between all Varnish worker threads.
        # Two initial cluster servers are provided; remaining servers are
        # automatically discovered.
        redis.init("cluster", "192.168.1.100:6379", 500, 0, 0, 16, 0, true, 100);
        redis.add_cserver("192.168.1.101:6379");
    }

    sub vcl_deliver {
        # SET internally routed to the destination server.
        redis.command("SET");
        redis.push("foo");
        redis.push("Hello world!");
        redis.execute();

        # GET internally routed to the destination server.
        redis.command("GET");
        redis.push("foo");
        redis.execute();
        set req.http.X-Foo = redis.get_string_reply();
    }

    sub vcl_fini {
        redis.fini();
    }

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

Dependencies:

* hiredis - minimalistic C Redis client library (https://github.com/redis/hiredis)

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
