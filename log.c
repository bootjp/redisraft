#include <unistd.h>
#include <string.h>

#include "redisraft.h"

RaftLog *RaftLogCreate(RedisRaftCtx *rr, const char *filename)
{
    int fd = open(filename, O_CREAT|O_RDWR|O_TRUNC, S_IRWXU|S_IRWXG);
    if (fd < 0) {
        LOG_ERROR("Failed to create Raft log: %s: %s\n", filename, strerror(errno));
        return NULL;
    }

    RaftLog *log = RedisModule_Calloc(1, sizeof(RaftLog));
    log->header = RedisModule_Calloc(1, sizeof(RaftLogHeader));
    log->fd = fd;

    log->header->version = RAFTLOG_VERSION;
    log->header->node_id = raft_get_nodeid(rr->raft);
    log->header->term = raft_get_current_term(rr->raft);
    log->header->entry_offset = sizeof(RaftLogHeader);

    if (write(log->fd, log->header, sizeof(RaftLogHeader)) < 0 ||
        fsync(log->fd) < 0) {

        LOG_ERROR("Failed to write Raft log header: %s: %s\n", filename, strerror(errno));
        
        RedisModule_Free(log->header);
        RedisModule_Free(log);

        log = NULL;
    }

    return log;
}

RaftLog *RaftLogOpen(RedisRaftCtx *rr, const char *filename)
{
    int fd = open(filename, O_RDWR);
    if (fd < 0) {
        LOG_ERROR("Failed top open Raft log: %s: %s\n", filename, strerror(errno));
        return NULL;
    }

    RaftLog *log = RedisModule_Calloc(1, sizeof(RaftLog));
    log->header = RedisModule_Calloc(1, sizeof(RaftLogHeader));
    log->fd = fd;

    int bytes;
    if ((bytes = read(log->fd, log->header, sizeof(RaftLogHeader))) < sizeof(RaftLogHeader)) {
        LOG_ERROR("Failed to read Raft log header: %s\n", bytes < 0 ? strerror(errno) : "file too short");
        goto error;
    }

    if (log->header->version != RAFTLOG_VERSION) {
        LOG_ERROR("Invalid Raft log version: %d\n", log->header->version);
        goto error;
    }

    return log;

error:
    RedisModule_Free(log->header);
    RedisModule_Free(log);
    return NULL;
}

int RaftLogLoadEntries(RedisRaftCtx *rr, RaftLog *log, int (*callback)(void **, raft_entry_t *), void *callback_arg)
{
    int ret = 0;

    if (lseek(log->fd, log->header->entry_offset, SEEK_SET) < 0) {
        LOG_ERROR("Failed to read Raft log: %s\n", strerror(errno));
        return -1;
    }

    do {
        raft_entry_t raft_entry;
        RaftLogEntry e;
        ssize_t nread = read(log->fd, &e, sizeof(RaftLogEntry));

        /* End of file? */
        if (!nread) {
            break;
        }

        /* Error? */
        if (nread < 0) {
            LOG_ERROR("Failed to read Raft log: %s\n", strerror(errno));
            ret = -1;
            break;
        }

        /* Set up raft_entry */
        raft_entry.term = e.term;
        raft_entry.id = e.id;
        raft_entry.type = e.type;
        raft_entry.data.len = e.len;
        raft_entry.data.buf = RedisModule_Alloc(e.len);

        /* Read data */
        uint32_t entry_len;
        struct iovec iov[2] = {
            { .iov_base = raft_entry.data.buf, .iov_len = e.len },
            { .iov_base = &entry_len, .iov_len = sizeof(entry_len) }
        };

        nread = readv(log->fd, iov, 2) != e.len + sizeof(entry_len);
        if (nread < e.len + sizeof(entry_len)) {
            LOG_ERROR("Failed to read Raft log entry: %s\n",
                    nread == -1 ? strerror(errno) : "truncated file");
            RedisModule_Free(raft_entry.data.buf);
            ret = -1;
            break;
        }

        int expected_len = sizeof(entry_len) + e.len + sizeof(RaftLogEntry);
        if (entry_len != expected_len) {
            LOG_ERROR("Invalid log entry size: %d (expected %d)\n", entry_len, expected_len);
            RedisModule_Free(raft_entry.data.buf);
            ret = -1;
            break;
        }
       
        int cb_ret = callback(callback_arg, &raft_entry);
        if (cb_ret < 0) {
            RedisModule_Free(raft_entry.data.buf);
            ret = cb_ret;
            break;
        }
        ret++;
    } while(1);

    return ret;
}

void RaftLogSetCommitIdx(RaftLog *log, uint32_t commit_idx)
{
    log->header->commit_idx = commit_idx;
}

void RaftLogSetVote(RaftLog *log, int vote)
{
    log->header->vote = vote;
}

void RaftLogSetTerm(RaftLog *log, int term)
{
    log->header->term = term;
}

bool RaftLogUpdate(RedisRaftCtx *rr, RaftLog *log)
{
    if (lseek(log->fd, 0, SEEK_SET) < 0) {
        return false;
    }

    if (write(log->fd, log->header, sizeof(RaftLogHeader)) < sizeof(RaftLogHeader) ||
        fsync(log->fd) < 0) {
        LOG_ERROR("Failed to update Raft log: %s", strerror(errno));
        return false;
    }

    return true;
}

bool RaftLogAppend(RedisRaftCtx *rr, RaftLog *log, raft_entry_t *entry)
{
    RaftLogEntry ent = {
        .term = entry->term,
        .id = entry->id,
        .type = entry->type,
        .len = entry->data.len
    };
    uint32_t entry_len = sizeof(ent) + entry->data.len + sizeof(entry_len);

    off_t pos = lseek(log->fd, 0, SEEK_END);
    if (pos < 0) {
        return false;
    }

    struct iovec iov[3] = {
        { .iov_base = &ent, .iov_len = sizeof(ent) },
        { .iov_base = entry->data.buf, .iov_len = entry->data.len },
        { .iov_base = &entry_len, .iov_len = sizeof(entry_len) }
    };

    ssize_t written = writev(log->fd, iov, 3);
    if (written < entry_len) {
        if (written == -1) {
            LOG_ERROR("Error writing Raft log: %s", strerror(errno));
        } else {
            LOG_ERROR("Incomplete Raft log write: %ld/%d bytes written", written, entry_len);
        }
       
        if (written != -1 && ftruncate(log->fd, pos) != -1) {
            LOG_ERROR("Failed to truncate partial entry!");
        }

        return false;
    }

    if (fsync(log->fd) < 0) {
        LOG_ERROR("Error syncing Raft log: %s", strerror(errno));
        return false;
    }

    return true;
}
