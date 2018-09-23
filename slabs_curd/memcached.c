#include "memcached.h"

#include <string>
#include <iomanip>
#include <iostream>
#include <cstdlib>

#include "util.h"
#include "trace.h"
#include "items.h"
#include "assoc.h"
#include "slabs.h"

extern uint64_t get_cas_id(void);
extern void item_remove(item *item);
extern int item_replace(item *old_it, item *new_it, const uint32_t hv);
extern enum hashfunc_type {
    JENKINS_HASH = 0,
    MURMUR3_HASH
};
extern int hash_init(enum hashfunc_type type);
extern void displayslabs();
extern void displayhashtable();
extern item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes);
extern int item_link(item *item);
extern void item_remove(item *item);
extern void item_unlink(item *item);
extern item* item_get(const char *key, const size_t nkey, conn *c, const bool do_update);

conn **conns;

// file scope variables
static int max_fds;

/**
 * Initializes the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the
 * maximum connection count is much higher than the actual number of 
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void) {
    int headroom = 10;    // account for extra unexpected open FDs
    
    max_fds = settings.maxconns + headroom;

    if ((conns = (conn **)calloc(max_fds, sizeof(conn *))) == NULL) {
        std::cout << "Failed to allocate connection structures" << std::endl;
        // This is unrecoverable so bail out early
        exit(1);
    }
}

static void settings_init(void) {
    settings.use_cas = true;
    
    settings.maxbytes = 64 * 1024 * 1024; // default is 64MB
    settings.maxconns = 1024; // to limit connections-related memory to about 5MB
    settings.verbose = 0;
    settings.evict_to_free = 1;     // push old items out of cache when memory runs out
    settings.factor = 1.25;
    settings.chunk_size = 48; // space for a modest key and value
    settings.item_size_max = 1024 * 1024; // The famous 1MB upper limit
    settings.slab_page_size = 1024 * 1024; // chunks are split from 1MB pages
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    settings.slab_reassign = true;
    settings.hashpower_init = 0;
    settings.num_threads = 4; // N workers
    settings.temp_lru = false;
    settings.temporary_ttl = 61;
    settings.inline_ascii_response = false;
    settings.lru_segmented = true;
    settings.hot_lru_pct = 20;
    settings.warm_lru_pct = 40;
    settings.tail_repair_time = TAIL_REPAIR_TIME_DEFAULT;
    settings.slab_automove = 1;
    settings.oldest_live = 0;
    settings.oldest_cas = 0; // supplements accuracy of oldest_live
}

/*
 * adds a delta value to a numeric item
 * 
 * c        connection requesting the operation
 * it       item to adjust
 * incr     true to increment value, false to decrement
 * delta    amount to adjust value by
 * buf      buffer to response string
 *
 * returns  a response string to send back to the client
 */
enum delta_result_type do_add_delta(conn *c, const char *key, const size_t nkey,
                                    const bool incr, const int64_t delta,
                                    char *buf, uint64_t *cas,
                                    const uint32_t hv) {
    char *ptr;
    uint64_t value;
    int res;
    item *it;

    it = do_item_get(key, nkey, hv, c, DONT_UPDATE);
    if (!it) {
        return DELTA_ITEM_NOT_FOUND;
    }

    /* Can't delta zero byte values. 2-byte are the "\r\n"
     * Also can't delta for chunked items. Too large to be a number.
     */
#ifdef EXTSTORE
    if (it->nbytes <= 2 || (it->it_flags & (ITEM_CHUNKED|ITEM_HDR)) != 0) {
#else
    if (it->nbytes <= 2 || (it->it_flags & (ITEM_CHUNKED)) != 0) {
#endif
        return NON_NUMERIC;
    }

    if (cas != NULL && *cas != 0 && ITEM_get_cas(it) != *cas) {
        do_item_remove(it);
        return DELTA_ITEM_CAS_MISMATCH;
    }

    ptr = ITEM_data(it);

    if (!safe_strtoull(ptr, &value)) {
        do_item_remove(it);
        return NON_NUMERIC;
    }

    if (incr) {
        value += delta;
        MEMCACHED_COMMAND_INCR(c->sfd, ITEM_key(it), it->nkey, value);
    } else {
        if (delta > value) {
            value = 0;
        } else {
            value -= delta;
        }
        MEMCACHED_COMMAND_DECR(c->sfd, ITEM_key(it), it->nkey, value);
    }

    pthread_mutex_lock(&c->thread->stats.mutex);
    if (incr) {
        c->thread->stats.slab_stats[ITEM_clsid(it)].incr_hits++;
    } else {
        c->thread->stats.slab_stats[ITEM_clsid(it)].decr_hits++;
    }
    pthread_mutex_unlock(&c->thread->stats.mutex);

    snprintf(buf, INCR_MAX_STORAGE_LEN, "%llu", (unsigned long long)value);
    res = strlen(buf);
    /* refcount == 2 means we are the only ones holding the item, and it is 
     * linked. We hold the item's lock in this function, so refcount cannot
     * increase.
     */
    if (res + 2 <= it->nbytes && it->refcount == 2) { // replace in-place
        /* When changing the value without replacing the item, we
         * need to update the CAS on the existing item.
         * We also need to fiddle it in the sizes tracker in case the tracking
         * was enabled at runtime, since it relies on the CAS value to know
         * whether to remove an item or not
         */
        item_stats_sizes_remove(it);
        ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
        item_stats_sizes_add(it);
        memcpy(ITEM_data(it), buf, res);
        memset(ITEM_data(it) + res, ' ', it->nbytes - res - 2);
        do_item_update(it);
    } else if (it->refcount > 1) {
        item *new_it;
        uint32_t flags;
        if (settings.inline_ascii_response) {
            flags = (uint32_t) strtoul(ITEM_suffix(it), (char **)NULL, 10);
        } else if (it->nsuffix > 0) {
            flags = *((uint32_t *)ITEM_suffix(it));
        } else {
            flags = 0;
        }
        new_it = do_item_alloc(ITEM_key(it), it->nkey, flags, it->exptime, res + 2);
        if (new_it == 0) {
            do_item_remove(it);
            return EOM;
        }
        memcpy(ITEM_data(new_it), buf, res);
        memcpy(ITEM_data(new_it) + res, "\r\n", 2);
        item_replace(it, new_it, hv);
        // Overwrite the older item's CAS with our new CAS since we're
        // returning the CAS of the old item below.
        ITEM_set_cas(it, (settings.use_cas) ? ITEM_get_cas(new_it) : 0);
        do_item_remove(new_it);     // release our reference
    } else {
        /* Should never get here. This means we somehow fetched and unlinked
         * item.TODO: Add a counter ?
         */
        if (settings.verbose) {
            fprintf(stderr, "Tried to do incr/decr on invalid item\n");
        }
        if (it->refcount == 1)
            do_item_remove(it);
        return DELTA_ITEM_NOT_FOUND;
    }

    if (cas) {
        *cas = ITEM_get_cas(it);    // swap the incoming CAS value
    }
    do_item_remove(it);     // release our reference
    return OK;
}

/** Destination must always be chunked.
 * This should be part of item.c
 */
static int _store_item_copy_chunks(item *d_it, item *s_it, const int len) {
    item_chunk *dch = (item_chunk *) ITEM_data(d_it);
    // Advance dch until we find free space
    while (dch->size == dch->used) {
        if (dch->next) {
            dch = dch->next;
        } else {
            break;
        }
    }

    if (s_it->it_flags & ITEM_CHUNKED) {
        int remain = len;
        item_chunk *sch = (item_chunk *) ITEM_data(s_it);
        int copied = 0;
        /**
         * Fills dch's to capacity, not straight copy sch in case data is
         * being added or removed (ie append/prepend)
         */
        while (sch && dch && remain) {
            assert(dch->used <= dch->size);
            int todo = (dch->size - dch->used < sch->used - copied)
              ? dch->size - dch->used : sch->used - copied;
            if (remain < todo) 
                todo = remain;
            memcpy(dch->data + dch->used, sch->data + copied, todo);
            dch->used += todo;
            copied += todo;
            remain -= todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, remain);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
            assert(copied <= sch->used);
            if (copied == sch->used) {
                copied = 0;
                sch = sch->next;
            }
        }
        // assert that the destination had enough space for source
        assert(remain == 0);
    } else {
        int done = 0;
        // Fill dch's via a non-chunked item.
        while (len > done && dch) {
            int todo = (dch->size - dch->used < len - done)
              ? dch->size - dch->used : len - done;
            // assert(dch->size - dch->used != 0);
            memcpy(dch->data + dch->used, ITEM_data(s_it) + done, todo);
            done += todo;
            dch->used += todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, len - done);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
        }
        assert(len == done);
    }
    return 0;
}

static int _store_item_copy_data(int comm, item *old_it, item *new_it, item *add_it) {
    if (comm == NREAD_APPEND) {
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, old_it, old_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, add_it, add_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(old_it), old_it->nbytes);
            memcpy(ITEM_data(new_it) + old_it->nbytes - 2 /* CRLF */, ITEM_data(add_it), add_it->nbytes);
        }
    } else {
        /* NREAD_PREPEND */
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, add_it, add_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, old_it, old_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(add_it), add_it->nbytes);
            memcpy(ITEM_data(new_it) + add_it->nbytes - 2 /* CRLF */, ITEM_data(old_it), old_it->nbytes);
        }
    }
    return 0;
}

/*
 * Stores an item in the cache according to the sematics of one of the set
 * commands. In threaded mode, this is protected by the cache lock.
 *
 * returns the state of storage.
 */
enum store_item_type do_store_item(item *it, int comm, conn *c, const uint32_t hv) {
    char *key = ITEM_key(it);
    item *old_it = do_item_get(key, it->nkey, hv, c, DONT_UPDATE);
    enum store_item_type stored = NOT_STORED;

    item *new_it = NULL;
    uint32_t flags;

    if (old_it != NULL && comm == NREAD_ADD) {
        /* add only adds a nonexistent item, but promote to head of LRU  */
        do_item_update(old_it);
    } else if (!old_it && (comm == NREAD_REPLACE
          || comm == NREAD_APPEND || comm == NREAD_PREPEND)) {
        /* replace only replaces an existing value; don't store */
    } else if (comm == NREAD_CAS) {
        /* validate cas operation */
        if (old_it == NULL) {
            // LRU expired
            stored = NOT_FOUND;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.cas_misses++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
        } else if (ITEM_get_cas(it) == ITEM_get_cas(old_it)) {
            // cas validates
            // it and old_it may belong to different classes.
            // I'm updating the stats for the one that's getting pushed out
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.slab_stats[ITEM_clsid(old_it)].cas_hits++;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            STORAGE_delete(c->thread->storage, old_it);
            item_replace(old_it, it, hv);
            stored = STORED;
        } else {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.slab_stats[ITEM_clsid(old_it)].cas_badval++;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            if (settings.verbose > 1) {
                fprintf(stderr, "CAS: failure: expected %llu, got %llu\n",
                        (unsigned long long)ITEM_get_cas(old_it),
                        (unsigned long long)ITEM_get_cas(it));
            }
            stored = EXISTS;
        }
    } else {
        int failed_alloc = 0;
        /*
         * Append - combine new and old record into single one. Here it's
         * atomic and thread-safe.
         */
        if (comm == NREAD_APPEND || comm == NREAD_PREPEND) {
            /*
             * Validate CAS
             */
            if (ITEM_get_cas(it) != 0) {
                // CAS much be equal
                if (ITEM_get_cas(it) != ITEM_get_cas(old_it)) {
                    stored = EXISTS;
                }
            }
#ifdef EXTSTORE
            if ((old_it->it_flags & ITEM_HDR) != 0) {
                /*
                 * block append/prepend from working with extstore-d items,
                 * also don't replace the header with the append chunk
                 * accidentally, so mark as a failed_alloc.
                 */
                failed_alloc = 1;
            } else 
#endif
            if (stored == NOT_STORED) {
                /* we have it and old_it here - alloc memory to hold both
                 * flags was already lost - so recover them from ITEM_suffix(it)
                 */
                if (settings.inline_ascii_response) {
                    flags = (uint32_t) strtoul(ITEM_suffix(old_it), (char **)NULL, 10);
                } else if (old_it->nsuffix > 0) {
                    flags = *((uint32_t *)ITEM_suffix(old_it));
                } else {
                    flags = 0;
                }

                new_it = do_item_alloc(key, it->nkey, flags, old_it->exptime, it->nbytes + 
                    old_it->nbytes - 2/* CRLF*/);

                /* copy data from it and old_it to new_it */
                if (new_it == NULL || _store_item_copy_data(comm, old_it, new_it, it) == -1) {
                    failed_alloc = 1;
                    stored = NOT_STORED;
                    // failed data copy, free up
                    if (new_it != NULL)
                        item_remove(new_it);
                } else {
                    it = new_it;
                }
            }
        }

        if (stored == NOT_STORED && failed_alloc == 0) {
            if (old_it != NULL) {
                STORAGE_delete(c->thread->storage, old_it);
                item_replace(old_it, it, hv);
            } else {
                do_item_link(it, hv);
            }

            c->cas = ITEM_get_cas(it);

            stored = STORED;
        }
    }

    if (old_it != NULL)
        do_item_remove(old_it);     // release our reference
    if (new_it != NULL)
        do_item_remove(new_it);

    if (stored == STORED) {
        c->cas = ITEM_get_cas(it);
    }
    LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE, NULL, stored, comm,
                ITEM_key(it), it->nkey, it->exptime, ITEM_clsid(it));

    return stored;
}

static void displayItem(item *it) {
    if (it == NULL) {
        return;
    }
    std::cout << "item next: " << it->next << std::endl;
    std::cout << "item prev: " << it->prev << std::endl;
    std::cout << "item h_next: " << it->h_next << std::endl;
    std::cout << "item time: " << it->time << std::endl;
    std::cout << "item exptime: " << it->exptime << std::endl;
    std::cout << "item nbytes: " << it->nbytes << std::endl;
    std::cout << "item refcount: " << it->refcount << std::endl;
    std::string flags = "";
    if (it->it_flags & ITEM_LINKED) {
        flags += "ITEM_LINKED|";
    }
    if (it->it_flags & ITEM_CAS) {
        flags += "ITEM_CAS|";
    }
    if (it->it_flags & ITEM_SLABBED) {
        flags += "ITEM_SLABBED|";
    }
    if (it->it_flags & ITEM_FETCHED) {
        flags += "ITEM_FETCHED|";
    }
    if (it->it_flags & ITEM_ACTIVE) {
        flags += "ITEM_ACTIVE|";
    }
    if (it->it_flags & ITEM_CHUNKED) {
        flags += "ITEM_CHUNKED|";
    }
    if (it->it_flags & ITEM_CHUNK) {
        flags += "ITEM_CHUNK|";
    }
    std::cout << "item flags: " << flags << std::endl;
    std::cout << "item nsuffix: " << std::hex << (uint32_t)it->nsuffix << std::endl;
    std::cout << "item clsid: " << std::hex << (uint32_t)it->slabs_clsid << std::endl;
    std::cout << "item nkey: " << std::hex << (uint32_t)it->nkey << std::dec << std::endl;
    std::cout << "item data: " << it->data << std::endl;
}

int main (int argc, char **argv) {
    bool preallocate = true;
    int retval = EXIT_SUCCESS;

    bool start_assoc_maint = true;

    uint32_t slab_sizes[MAX_NUMBER_OF_SLAB_CLASSES];
    bool use_slab_sizes = false;

#ifdef EXTSTORE 
    void *storage = NULL;
#endif 

    // init settings
    settings_init();

    // hash
    enum hashfunc_type hash_type = MURMUR3_HASH;

    if (hash_init(hash_type) != 0) {
        fprintf(stderr, "Failed to initialize has_algorithm");
        exit(0);
    }

    // initialize other stuff
    assoc_init(settings.hashpower_init);
    conn_init();
    slabs_init(settings.maxbytes, settings.factor, preallocate,
                use_slab_sizes ? slab_sizes : NULL);

#ifdef EXTSTORE 
    memcached_thread_init(settings.num_threads, storage);
#else
    memcached_thread_init(settings.num_threads, NULL);
#endif

    if (start_assoc_maint && start_assoc_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    // display code start
    displayMemory();
    displayslabs();
    displayhashtable();
    // display code end
    
    // curd code start
    std::cout << std::endl;
    auto it = item_alloc("memcached", sizeof("memcached"), 0, 1536681392, 16);
    displayItem(it);
    auto getItem = item_get("memcached", sizeof("memcached"), conns[0], false);
    if (getItem == NULL) {
        std::cout << "get item is null" << std::endl;
    } else {
        displayItem(it);
    }
    std::cout << item_link(it) << std::endl << std::endl;
    displayItem(it);
    item_remove(it);
    std::cout << std::endl;
    displayItem(it);
    std::cout << std::endl;
    item_unlink(it);
    std::cout << std::endl;
    displayItem(it);
    // curd code end

    // display code start
    displayMemory();
    displayslabs();
    displayhashtable();
    // display code end
    
    stop_assoc_maintenance_thread();

    return retval;
}
