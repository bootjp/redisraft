# Redis Raft Module

This is an **experimental, work-inprogress** Redis module that implements the
[Raft Consensus Algorithm](https://raft.github.io/) as a Redis module.

Using this module it is possible to form a cluster of Redis servers which
provides the fault tolerance properties of Raft:

1. Leader election.  The servers elect a single leader at a time, and only the
   leader is willing to accept user requests.  Other members of the cluster
   will reply with a redirect message.
2. User requests are replied only after they have been replicated to a majority
   of the cluster.
3. Cluster configuration is dynamic and it is possible to add or remove members
   on the fly.

## Getting Started

### Building

The module is mostly self contained and comes with its dependencies as git
submodules under `deps`.  To compile you will need:
* Obvious build essentials (compiler, GNU make, etc.)
* CMake
* GNU autotools (autoconf, automake, libtool)

### Testing

The module includes a minimal set of unit tests and integration tests.  To run
them you'll need:
* lcov (for coverage analysis, on Linux)
* Python and a few packages (e.g. nose, redis, etc.)
* redis-server in your PATH, or in `../redis/src`.

To run tests in a Python virtualenv, follow these steps:

```
$ mkdir -p .env
$ virtualenv .env
$ . .env/bin/active
$ pip install -r tests/integration/requirements.txt
$ make tests
```

Integration tests are based on Python nose, and specific parameters can be
provided to configure tests.  For example, running a single test with
no logging capture, so output is printed in runtime:

```
NOSE_OPTS="-v --nologcapture --logging-config=tests/integration/logging.ini --tests tests/integration/test_snapshots.py:test_new_snapshot_with_old_log" make integration-tests
```

### Jepsen

The module ships with a basic Jepsen test to verify its safety.  It is set up to
execute a 5-node cluster + a Jepsen control node, based on the Jepsen Docker
Compose setup.

First, you'll need to build a tarball that contains Redis + the module, compiled
for a debian image used for Jepsen.  To do that, run:

```
./jepsen/docker/build_dist.sh
```

This should result with a `jepsen/docker/dist` directory being created with a
single tarball.

To start Jepsen:

```
cd jepsen/docker
./up.sh
```

This will launch Jepsen's built-in web server on `http://localhost:8080`, but do
nothing else.  To start an actual test, use a command such as:

```
docker exec -ti jepsen-control bash
lein run test --time-limit 60 --concurrency 200
```

### Starting a cluster

To create a three node cluster, start the first node and initialize the
cluster:

```
redis-server \
    --port 5001 --dbfilename raft1.rdb \
    --loadmodule <path-to>/redisraft.so \
        id=1 raft-log-filename=raftlog1.db addr=localhost:5001
redis-cli -p 5001 raft.cluster init
```

Then start the second node and make it join the cluster:

```
redis-server \
    --port 5002 --dbfilename raft2.rdb \
    --loadmodule <path-to>/redisraft.so \
        id=2 raft-log-filename=raftlog2.db addr=localhost:5002
redis-cli -p 5002 raft.cluster join localhost:5001
```

And the third node:

```
redis-server \
    --port 5003 --dbfilename raft3.rdb \
    --loadmodule <path-to>/redisraft.so \
        id=3 raft-log-filename=raftlog3.db addr=localhost:5003
redis-cli -p 5003 raft.cluster join localhost:5001
```

To query the cluster state:

```
redis-cli --raw -p 5001 RAFT.INFO
```

And to submit a Raft operation:

```
redis-cli -p 5001 RAFT SET mykey myvalue
```

## Issues and Limitations

### Transparency

Currently the module is not transparent, as all Redis commands need to be
prefixed with `RAFT` to be processed properly.

This will change when the Redis Module API has support for command hooking and
make it possible to intercept built-in commands.

For now it is possible to use an [experimental patch that offers a command
filtering
API](https://github.com/yossigo/redis/commit/234d25ea0adfaa724fbfac41a2d672d0f556d42a)
and enable it on the module side by un-commenting `-DUSE_COMMAND_FILTER` in the
Makefile.

### Follower Proxy

By default, an attempt to send a `RAFT` command to a follower node will result
with a `-MOVED` response that includes the address and port of the leader.

It is possible to enable the `follower-proxy` configuration setting (see below),
so followers will instead attempt to deliver the command to the leader over an
established connection.  If successful, the response is then proxied back to the
user when received from the leader.  If no response is received and the
connection is dropped, a `-TIMEOUT` error is returned instead, indicating the
status of the request is unknown (it may or may not have been received).

**NOTE: The Proxy mechanism is quite limited, as it reuses existing connections
and does not maintain a connection pool, etc.  It is mainly used as an easier
way to run safety tests against the cluster.**

### Supported Commands

Commands passed to the `RAFT` command are naively added to the Raft log and
later passed to Redis when committed.

This works well for most simple commands manipulating data types, but may result
with unexpected/undesired results in other cases.  For example:

- `MUTLI`/`EXEC` cannot be passed to `RAFT`, and will fail to offer atomic
  execution.
- Blocking commands are not supported, as they cannot be relayed by the module
  to Redis (they will not block).
- Streams are not supported.
- Pubsub are not supported.

### Read Safety

In a Raft cluster, reads may be fulfilled in different levels of safety:
1. Quorum reads, only processed by the leader after confirming a majority
   still considers it a leader (i.e. not stale).
2. Potentially stale reads, processed by the leader without the above
   confirmation.  The risk here is that another leader may have **recently**
   been elected and the read would be stale.
3. Unsafe reads, which may be fulfilled from any node.

Unsafe reads can be executed by bypassing the Raft module and directly reading
from Redis.

Quorum reads are enabled by default and controlled by the `quorum-reads` config
parameter.  When a read-only command is detected, it is not replicated through
the log but instead put on a local read queue.  A heartbeat (empty
AppendEntries) is then broadcast, and the command is only served if/when a
majority responds.

If quorum reads are disabled, Redis read-only commands are executed immediately
if the local node is a leader.

## Configuration

The Raft module has its own set of configuration parameters, which can be
controlled in different ways:

1. Passed as `param`=`value` pairs as module arguments.
2. Using `RAFT.CONFIG SET` and `RAFT.CONFIG GET`, which behave the same as as
   Redis `CONFIG` commands.

The following configuration parameters are supported:

| Parameter               | Description |
| ---------               | ----------- |
| id                      | A unique numeric ID of the node in the cluster. *Default: none, required.* |
| addr                    | Address and port the node advertises itself on. *Default: non-local interfce address and the Redis port.* |
| raft-log-filename       | Raft log filename. *Default: redisraft.db.* |
| raft-interval           | Interval (in ms) in which Raft wakes up and handles chores (e.g. send heartbeat AppendEntries to nodes, etc.). *Default: 100*. |
| request-timeout         | Amount of time (in ms) before an AppendEntries request is resent. *Default: 250*. |
| election-timeout        | Amount of time (in ms) after the last AppendEntries, before we assume a leader is down. *Default: 500*. |
| reconnect-interval      | Amount of time (in ms) to wait before retrying to establish a connection with another node. *Default: 100*. |
| follower-proxy          | If `yes`, follower nodes proxy commands to the leader.  Otherwise, a `-MOVED` response is returned with the address and port of the leader. *Default: no.* |
| raft-log-max-file-size  | Maximum allowed Raft log file size, before compaction is initiated. *Default: 64MB*. |
| raft-log-max-cache-size | Maximum size of in-memory Raft log cache. *Default: 8MB*. |
| raft-log-fsync          | Toggles the use of fsync when appending entries to the log. *Default: true*. |
| quorum-reads            | Toggles safe quorum reads. *Default: true*. |

# Implementation Details

## Overview

The module uses a [standalone C library implementation of
Raft](https://github.com/willemt/raft) by Willem-Hendrik Thiart for all Raft
algorithm related work.

A single `RAFT` command is implemented as a prefix command for users to submit
requests to the Raft log.  This triggers the following series of events:

1. The command is appended to the local Raft log (in memory cache and file).
2. The log is replicated to the majority of cluster members.  This is done by
   the Raft module communicating with the other Raft modules using
   module-specific commands.
3. When a majority has been reached and Raft determines the entry can be
   committed, it is executed locally as a regular Redis command and the
   response is sent to the user.

Raft communication between cluster members is handled by `RAFT.AE` and
`RAFT.REQUESTVOTE` commands which are also implemented by the module.

The module starts a background thread which handles all Raft related tasks,
such as:
* Maintain connections with all cluster members
* Periodically send heartbeats (leader) or initiate vote if heartbeats are not
  seen (follower/candidate).
* Process committed entries (deliver to Redis through a thread safe context)

All received Raft commands are placed on a queue and handled by the Raft
thread itself, using the blocking API and a thread safe context.

## Node Membership

When a new node starts up, it can follow one of the following flows:

1. Start as the first node of a new cluster.
2. Start as a new node of an existing cluster (with a new unique ID).
   Initially it will be a non-voting node, only receiving logs (or snapshot).
3. Start as an existing cluster node which recovers from crash.  Typically
   this is done by loading persistent data from disk.

Configuration changes are propagated as special Raft log entries, as described
in the Raft paper.

The trigger for configuration changes is provided by `RAFT.NODE ADD` and
`RAFT.NODE REMOVE` commands.

*NOTE: Currently membership operations are node-centric. That is, a node is
started with module arguments that describe how it should behave.  For example,
a `RAFT.CLUSTER JOIN` is invoked on a new node in order to initiate a connection
to the leader and execute a `RAFT.NODE ADD` command.

While there are some benefits to this approach, it may make more sense to change
to a cluster-centric approach which is the way Redis Cluster does things.*

## Persistence

The Raft Log is persisted to disk in a dedicated log file managed by the module.
In addition, an in-memory cache of recent entries is maintained in order to
optimize log access.

The file format is based on RESP encoding and is similar to an AOF file.  It
begins with a header entry that stores the Raft state at the time the log was
created, followed by a list of entries.

The header entry may be updated to persist additional data such as voting
information.  For this reason, the entry sized is fixed.

In addition, the module maintains a simple index file to store the 64-bit
offsets of every entry written to the log.

## Log Compaction

Raft defines a mechanism for compaction of logs by storing and exchanging
snapshots.  The snapshot is expected to be persisted just like the log, and
include information that was removed from the log during compaction.

### Compaction & Snapshot Creation

When the Raft modules determines it needs to perform log compaction, it does the
following:

First, a child process is forked and:
1. Performs a Redis `SAVE` operation after modifying the `dbfilename`
   configuration, so a temporary file is created.
2. Iterates the Raft log and creates a new temporary Raft log with
   only the entries that follow the snapshot.
3. Exits and reports success to the parent.

The parent detects that the child has completed and:
1. Renames the temporary snapshot (rdb) file so it overwrites the
   existing one.
2. Appends all Raft log entries that have been received since the
   child was forked to the temporary Raft log file.
3. Renames the temporary Raft log so it overwrites the existing one.

Note that while the above is not atomic, operations are ordered such that a
failure at any given time would not result with data loss.

### Snapshot Delivery

When a Raft follower node lags behind and requires log entries that have been
compacted, a snapshot needs to be delivered instead:

1. Leader decides it needs to send a snapshot to a remote node.
2. Leader sends a `RAFT.LOADSNAPSHOT` command, which includes the
   snapshot (RDB file) as well as *last-included-term* and
   *last-included-index*.
3. Follower may respond in different ways:
   * `1` indicates snapshot was successfully loaded.
   * `0` indicates the local index already matches the required snapshot index so
     nothing needs to be done.
   * `-LOADING` indicates snapshot loading is already in progress.

*NOTE: Because of the store-and-forward implementation in Redis, this is not
very efficient and will fail on very large datasets. In the future this should
be optimized*.

## Roadmap

- [x] Decouple log implementation, to allow storing most of the log on disk and
      only a recent cache in memory (Raft lib).
- [x] Optimize reads, so they are not added as log entries (Raft lib).
- [x] More friendly membership management through Redis commands, to avoid
      changing process arguments.
- [x] Optimize memory management (Raft lib).
- [ ] Add NO-OP log entry when starting up, to force commit index computing.
- [ ] Latency optimizations through better concurrency (batch operations,
      distribute entries while syncing to disk, etc.).
- [ ] Improve automatic proxying performance.
- [ ] Improve debug logging (Redis Module API).
- [ ] Batch log operations (Raft lib).
- [ ] Cleaner snapshot RDB loading (Redis Module API).
- [ ] Stream snapshot data on LOAD.SNAPSHOT (hiredis streaming support).
