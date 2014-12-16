==========
vmod_redis
==========

.. image:: https://travis-ci.org/carlosabalde/libvmod-redis.svg?branch=3.0
    :target: https://travis-ci.org/carlosabalde/libvmod-redis

--------------------
Varnish Redis Module
--------------------

:Author: Carlos Abalde
:Date: 2014-12-14
:Version: 0.1.3
:Manual section: 3

SYNOPSIS
========

import redis;

::

    # Configuration.
    Function VOID init(TAG, LOCATION, TIMEOUT, TTL, SHARED_CONTEXTS, MAX_CONTEXTS)
    Function VOID add_server(TAG, LOCATION, TIMEOUT, TTL)
    Function VOID add_cserver(LOCATION)

    # Command execution.
    Function VOID command(COMMAND)
    Function VOID server(TAG)
    Function VOID push(ARGUMENT)
    Function VOID execute()

    # Access to replies.
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

DESCRIPTION
===========

VMOD using the synchronous hiredis library API (https://github.com/redis/hiredis) to access Redis servers from VCL.

Highlights:

* **Full support for execution of LUA scripts** (i.e. ``EVAL`` command), including optimistic automatic execution of ``EVALSHA`` commands.
* **All Redis reply data types are supported**, including partial support to access to components of simple (i.e. not nested) array replies.
* **Redis pipelines are not (and won't be) supported**. LUA scripting, which is fully supported by the VMOD, it's a much more flexible alternative to pipelines for atomic execution and minimizing latency. Pipelines are hard to use and error prone, specially when using the ``WATCH`` command.
* **Support for classic Redis deployments** using multiple Redis servers (replicated or standalone) **and for clustered deployments based on Redis Cluster**.
* **Support for multiple Redis connections**, local to each Varnish worker thread, or shared using one or more pools.

Looking for official support for this VMOD? Please, contact Allenta Consulting (http://www.allenta.com), the Varnish Software integration partner for Spain and Portugal (https://www.varnish-software.com/partner/allenta-consulting).

EXAMPLES
========

Single server
-------------

::

    sub vcl_init {
        # VMOD configuration: simple case, keeping up to one Redis connection
        # per Varnish worker thread.
        redis.init("main", "192.168.1.100:6379", 500, 0, false, 1);
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
        redis.init("master", "192.168.1.100:6379", 500, 0, false, 2);
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
        redis.init("cluster", "192.168.1.100:6379", 500, 0, true, 100);
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


CONFIGURATION FUNCTIONS
=======================

init
----

Prototype
        ::

                init(STRING tag, STRING location, INT timeout, INT ttl, BOOL shared_contexts, INT max_contexts)
Arguments
    tag: name tagging the Redis server in some category (e.g. ``main``, ``master``, ``slave``, etc.). When using the reserved tag ``cluster`` the VMOD internally enables the
    Redis Cluster support, automatically discovering other servers in the cluster using the command ``CLUSTER SLOTS``.

    location: Redis connection string. Both host + port and UNIX sockets are supported. If this is a Redis Cluster server only host + port format is allowed.

    timeout: connection timeout (milliseconds) to the Redis server. If Redis Cluster support has been enabled all servers in the cluster will use this timeout.

    ttl: TTL (seconds) of Redis connections (0 means no TTL). Once the TTL of a connection is consumed, the module transparently reestablishes it. See "Client timeouts" in http://redis.io/topics/clients for extra information. If Redis Cluster support has been enabled all servers in the cluster will use this TTL.

    shared_contexts: if enabled, Redis connections are not local to Varnish worker threads, but shared by all threads using one or more pools.

    max_contexts: when ``shared_contexts`` is disabled, this option sets the maximum number of Redis connections per Varnish worker thread. Each thread keeps up to one connection per tag. If more than one tag is available, incrementing this limit allows recycling of Redis connections. When ``shared_contexts`` is enabled, the VMOD created one pool per tag; this option sets the maximum number of Redis connections per pool. Note that when Redis Cluster support is enabled, each server is the cluster is internally labeled by the VMOD with a different tag (i.e. each server in the cluster has its own pool of Redis connections).
Return value
    VOID
Description
    Initializes the Redis module.
    Must be called during the ``vcl_init`` phase.

add_server
----------

Prototype
        ::

                add_server(STRING tag, STRING location, INT timeout, INT ttl)
Arguments
    tag: name tagging the Redis server in some category (e.g. ``main``, ``master``, ``slave``, etc.). Using the reserved tag ``cluster`` is not allowed.

    location: Redis connection string. Both host + port and UNIX sockets are supported.

    timeout: connection timeout (milliseconds) to the Redis server.

    ttl: TTL (seconds) of Redis connections (0 means no TTL). Once the TTL of a connection is consumed, the module transparently reestablishes it. See "Client timeouts" in http://redis.io/topics/clients for extra information.
Return value
    VOID
Description
    Adds an extra Redis server.
    Must be used during the ``vcl_init`` phase.

    Use this feature (1) when using master-slave replication; or (2) when using multiple independent servers; or (3) when using some kind of proxy assisted partitioning (e.g. https://github.com/twitter/twemproxy) and more than one proxy is available.

    When a command is submitted using ``redis.execute()`` and more that one Redis server is available, the destination server is selected according with the tag specified with `redis.server()`. If not specified and Redis Cluster support hasn't been enabled, a randomly selected connection will be used (if the worker thread / corresponding pool already has any Redis connection established and available), or a new connection to a randomly selected server will be established.

add_cserver
-----------

Prototype
        ::

                add_cserver(STRING location)
Arguments
    location: Redis connection string. Only host + port format is allowed.
Return value
    VOID
Description
    Adds an extra Redis Cluster server.
    Must be used during the ``vcl_init`` phase.

    This feature is only available once Redis Custer support has been enabled when calling ``redis.init()``. Other servers in the cluster are automatically discovered by the VMOD using the ``CLUSTER SLOTS`` commands. Anyway, knowing more cluster servers during startup increases the chances of discover the cluster topology if some server is failing.

COMAND EXECUTION FUNCTIONS
==========================

command
-------

Prototype
        ::

                command(STRING name)
Arguments
    name: name of the Redis command to be executed.
Return value
    VOID
Description
    Enqueues a Redis command (only the name of the command) for further execution.
    Arguments should be enqueued separately calling one or more times to the ``redis.push()`` function.

    On execution time, ``EVAL`` commands are internally replace by ``EVALSHA`` commands, which fallback to the original ``EVAL`` command if the Redis server returns a ``NOSCRIPT`` error (see http://redis.io/commands/eval).

server
------

Prototype
        ::

                server(STRING tag)
Arguments
    tag: tag of the Redis server a previously enqueued Redis command will be delivered to (e.g. ``main``, ``master``, ``slave``, ``cluster``, etc.).
Return value
    VOID
Description
    Selects the type of Redis server a previously enqueued Redis command will be delivered to.

    If not specified and Redis Cluster support hasn't been enabled, a randomly selected connection / server will be used (see ``redis.add_server()`` for extra information).

push
----

Prototype
        ::

                push(STRING arg)
Arguments
    name: argument of a previously enqueued Redis command.
Return value
    VOID
Description
    Executes an argument of a previously enqueued Redis command.

execute
-------

Prototype
        ::

                execute()
Return value
    VOID
Description
    Executes a previously enqueued Redis command.

ACCESS TO REPLY FUNCTIONS
=========================

reply_is_error
--------------

Prototype
        ::

                reply_is_error()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command using ``redis.execute()`` returned an error reply.

reply_is_nil
------------

Prototype
        ::

                reply_is_nil()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command using ``redis.execute()`` returned a nil reply.

reply_is_status
---------------

Prototype
        ::

                reply_is_status()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command using ``redis.execute()`` returned a status reply.

reply_is_integer
----------------

Prototype
        ::

                reply_is_integer()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command ``redis.execute()`` returned an integer reply.

reply_is_string
---------------

Prototype
        ::

                reply_is_string()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command ``redis.execute()`` returned a string reply.

reply_is_array
--------------

Prototype
        ::

                reply_is_array()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command using ``redis.execute()`` returned an array reply.

get_reply
---------

Prototype
        ::

                get_reply()
Return value
    STRING
Description
    Returns a string representation of the reply of a previously executed Redis command using ``redis.execute()``.
    Do not use this function to access to array replies.

get_error_reply
---------------

Prototype
        ::

                get_error_reply()
Return value
    STRING
Description
    If a previously executed Redis command using ``redis.execute()`` returned an error reply, this function returns a string representation of that reply.

get_status_reply
----------------

Prototype
        ::

                get_status_reply()
Return value
    STRING
Description
    If a previously executed Redis command using ``redis.execute()`` returned a status reply, this function returns a string representation of that reply.

get_integer_reply
-----------------

Prototype
        ::

                get_integer_reply()
Return value
    INT
Description
    If a previously executed Redis command using ``redis.execute()`` returned an integer reply, this function returns an integer representation of that reply.

get_string_reply
----------------

Prototype
        ::

                get_string_reply()
Return value
    STRING
Description
    If a previously executed Redis command using ``redis.execute()`` returned string reply, this function returns a string representation of that reply.

get_array_reply_length
----------------------

Prototype
        ::

                get_array_reply_length()
Return value
    INT
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns the number of elements in that reply.

array_reply_is_error
--------------------

Prototype
        ::

                array_reply_is_error(INT index)
Return value
    BOOL
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns TRUE if the nth element in that reply is an error reply (nested arrays are not supported).

array_reply_is_nil
------------------

Prototype
        ::

                array_reply_is_nil(INT index)
Return value
    BOOL
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns TRUE if the nth element in that reply is a nil reply (nested arrays are not supported).

array_reply_is_status
---------------------

Prototype
        ::

                array_reply_is_status(INT index)
Return value
    BOOL
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns TRUE if the nth element in that reply is a status reply (nested arrays are not supported).

array_reply_is_integer
----------------------

Prototype
        ::

                array_reply_is_integer(INT index)
Return value
    BOOL
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns TRUE if the nth element in that reply is an integer reply (nested arrays are not supported).

array_reply_is_string
---------------------

Prototype
        ::

                array_reply_is_string(INT index)
Return value
    BOOL
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns TRUE if the nth element in that reply is a string reply (nested arrays are not supported).

array_reply_is_array
--------------------

Prototype
        ::

                array_reply_is_array(INT index)
Return value
    BOOL
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns TRUE if the nth element in that reply is an array reply (nested arrays are not supported).

get_array_reply_value
---------------------

Prototype
        ::

                get_array_reply_value(INT index)
Return value
    STRING
Description
    If a previously executed Redis command using ``redis.execute()`` returned an array reply, this function returns a string representation of the nth element in that reply (nested arrays are not supported).

OTHER FUNCTIONS
===============

free
----

Prototype
        ::

                free()
Return value
    VOID
Description
    Frees memory internally used by Redis commands an replies.
    It's recommended to use this function, but if not called this will be handled automatically during the next call to ``redis.command()``.

fini
----

Prototype
        ::

                fini()
Return value
    VOID
Description
    Closes all established Redis connections in shared pools.
    Must be used during the ``vcl_fini`` phase.
    It's recommended to use this function, but if not called this will be handled automatically during the unload of the VCL using the VMOD.

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

Dependencies:

* hiredis - minimalistic C Redis client library (https://github.com/redis/hiredis)

COPYRIGHT
=========

This document is licensed under the same license as the libvmod-redis project. See LICENSE for details.

Implementation of the SHA-1 and CRC-16 cryptographic hash functions embedded in this VMOD (required to the optimistic execution of ``EVALSHA`` commands, and to the Redis Cluster slot calculation, respectively) are borrowed from the Redis implementation:

* http://download.redis.io/redis-stable/src/sha1.c
* http://download.redis.io/redis-stable/src/sha1.h
* http://download.redis.io/redis-stable/src/config.h
* https://github.com/antirez/redis/blob/unstable/src/crc16.c

Copyright (c) 2014 Carlos Abalde <carlos.abalde@gmail.com>
