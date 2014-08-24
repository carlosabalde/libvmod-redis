==========
vmod_redis
==========

--------------------
Varnish Redis Module
--------------------

:Author: Carlos Abalde
:Date: 2014-08-22
:Version: 0.1
:Manual section: 3

SYNOPSIS
========

import redis;

DESCRIPTION
===========

VMOD using the synchronous hiredis library API (https://github.com/redis/hiredis) to access Redis servers from VCL.

Highlights:

* **All Redis commands are supported**. Two VMOD APIs are provided: ``redis.call()`` for simple commands, and ``redis.command()`` + ``redis.push()`` + ``redis.execute()`` for advanced execution.
* **Full support for execution of LUA scripts** (i.e. ``EVAL`` command), including optimistic automatic execution of ``EVALSHA`` commands.
* **All Redis reply data types are supported**, including partial support to access to components of simple (i.e. not nested) array replies.
* **Redis pipelines are not (and won't be) supported**. LUA scripting, which is fully supported by the VMOD, it's a much more flexible alternative to pipelines for atomic execution and minimizing latency. Pipelines are hard to use and error prone, specially when using the ``WATCH`` command.

EXAMPLES
========

::

    sub vcl_init {
        # VMOD configuration.
        redis.init("127.0.0.1", 6379, 500, 0);
    }

    sub vcl_deliver {
        # Simple command execution.
        redis.call("SET foo hello");
        redis.call("GET foo");
        set req.http.X-Foo = redis.get_string_reply();

        # Simple command execution using placeholders.
        set req.http.X-Key = "foo";
        set req.http.X-Value = "Hello world!";
        redis.call("SET %s %s" + req.http.X-Key + req.http.X-Value);
        redis.call("GET %s" + req.http.X-Key);
        set req.http.X-Foo = redis.get_string_reply();

        # Advanced command execution.
        redis.command("SET");
        redis.push("bar");
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

        # Array replies.
        redis.call("MGET foo bar");
        if ((redis.reply_is_array()) &&
            (redis.get_array_reply_length() == 2)) {
            set resp.http.X-Foo = redis.get_array_reply_value(0);
            set resp.http.X-Bar = redis.get_array_reply_value(1);
        }
    }

CONFIGURATION FUNCTIONS
=======================

init
----

Prototype
        ::

                init(STRING host, INT port, INT timeout, INT ttl)
Arguments
    host: host where the Redis server is running.

    port: port where the Redis server is running.

    timeout: connection timeout (milliseconds) to the Redis server.

    ttl: TTL (seconds) of Redis connections (0 means no TTL). Once the TTL of a connection is consumed, the module transparently reestablishes it. See "Client timeouts" in http://redis.io/topics/clients for extra information.
Return value
    VOID
Description
    Initializes the Redis module.
    Must be used during the ``vcl_init`` phase.
    If not called some default values will be used.

SIMPLE COMAND EXECUTION FUNCTIONS
=================================

call
----

Prototype
        ::

                call(STRING_LIST command)
Arguments
    command: full Redis command.
Return value
    VOID
Description
    Executes a simple Redis command.
    Reply can be fetched with ``redis.reply_is_.*()`` and ``redis.get_.*()`` functions.
    This function implements an ugly hack based on the VMOD STRING_LIST data type in order to support some formatting placeholders like ``%s``, ``%d``, etc.
    Please, use ``redis.command()`` + ``redis.push()`` + ``redis.execute()`` for extra flexibility and optimistic execution of ``EVALSHA`` commands.

ADVANCED COMAND EXECUTION FUNCTIONS
===================================

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
    On execution time, ``EVAL`` commands are internally replace by ``EVALSHA`` commands, which fallback to the original ``EVAL`` command if the Redis server returns a NOSCRIPT error (see http://redis.io/commands/eval).


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
    Returns TRUE if a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned an error reply.

reply_is_nil
------------

Prototype
        ::

                reply_is_nil()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned a nil reply.

reply_is_status
---------------

Prototype
        ::

                reply_is_status()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned a status reply.

reply_is_integer
----------------

Prototype
        ::

                reply_is_integer()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned an integer reply.

reply_is_string
---------------

Prototype
        ::

                reply_is_string()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned a string reply.

reply_is_array
--------------

Prototype
        ::

                reply_is_array()
Return value
    BOOL
Description
    Returns TRUE if a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned an array reply.

get_reply
---------

Prototype
        ::

                get_reply()
Return value
    STRING
Description
    Returns a string representation of the reply of a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``).
    Do not use this function to access to array replies.

get_error_reply
---------------

Prototype
        ::

                get_error_reply()
Return value
    STRING
Description
    If a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned an error reply, this function returns a string representation of that reply.

get_status_reply
----------------

Prototype
        ::

                get_status_reply()
Return value
    STRING
Description
    If a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned a status reply, this function returns a string representation of that reply.

get_integer_reply
-----------------

Prototype
        ::

                get_integer_reply()
Return value
    INT
Description
    If a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned an integer reply, this function returns an integer representation of that reply.

get_string_reply
----------------

Prototype
        ::

                get_string_reply()
Return value
    STRING
Description
    If a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned string reply, this function returns a string representation of that reply.

get_array_reply_length
----------------------

Prototype
        ::

                get_array_reply_length()
Return value
    INT
Description
    If a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned an array reply, this function returns the number of elements in that reply.

get_array_reply_value
---------------------

Prototype
        ::

                get_array_reply_value(INT index)
Return value
    STRING
Description
    If a previously executed Redis command (using ``redis.call()`` or ``redis.execute()``) returned an array reply, this function returns a string representation of the nth element in that reply (nested arrays are not supported).

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
    If this function is not called this will be handled automatically.

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

Usage::

 ./configure VARNISHSRC=DIR [VMODDIR=DIR]

``VARNISHSRC`` is the directory of the Varnish source tree for which to compile your VMOD. Both the ``VARNISHSRC`` and ``VARNISHSRC/include`` will be added to the include search paths for your module.

Optionally you can also set the VMOD install directory by adding ``VMODDIR=DIR`` (defaults to the pkg-config discovered directory from your Varnish installation).

Make targets:

* make - builds the VMOD
* make install - installs your VMOD in ``VMODDIR``
* make check - runs the unit tests in ``src/tests/*.vtc``

Dependencies:

* hiredis - minimalistic C Redis client library (https://github.com/redis/hiredis)

COPYRIGHT
=========

This document is licensed under the same license as the libvmod-redis project. See LICENSE for details.

Implementation of the SHA-1 cryptographic hash function embedded in this VMOD (required to the optimistic execution of ``EVALSHA`` commands) is borrowed from the Redis server implementation:

* http://download.redis.io/redis-stable/src/sha1.c
* http://download.redis.io/redis-stable/src/sha1.h
* http://download.redis.io/redis-stable/src/config.h

Copyright (c) 2014 Carlos Abalde <carlos.abalde@gmail.com>
