#pragma once

#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "logger.h"

#define ENDIAN_BIG 1

/* Maximum length of a key. */
#define KEY_MAX_LENGTH 250

/* initial power multiplier for the hash table */
#define HASHPOWER_DEFAULT 16
#define HASHPOWER_MAX 32

/*
 * We only reposition items in the LRU queue if they haven't been repositioned
 * in this many seconds. That saves use from churning on frequently-accessed
 * items.
 */
#define ITEM_UPDATE_INTERVAL 60

/* Slab sizing definitions. */
#define POWER_SMALLEST  1
#define POWER_LARGEST   256 /* actual cap is 255*/
#define SLAB_GLOBAL_PAGE_POOL 0 /* magic slab class for storing pages for reassignment */
#define CHUNK_ALIGN_BYTES 8
/* slab class max is a 6-bit number, -1. */
#define MAX_NUMBER_OF_SLAB_CLASSES (63+1)

/**
 * How long an object can reasonably be assumed to be locked before
 * harvesting it on a low memory conditio. Default: disabled
 */
#define TAIL_REPAIR_TIME_DEFAULT 0

/**
 * waring: don't use these macros with a function, as it evals its arg twice
 */
#define ITEM_get_cas(i) (((i)->it_flags & ITEM_CAS) ? \
        (i)->data->cas : (uint64_t)0)

#define ITEM_set_cas(i, v) { \
    if ((i)->it_flags & ITEM_CAS) { \
        (i)->data->cas = v; \
    } \
}

#define ITEM_key(item) (((char*)&((item)->data)) \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_suffix(item) ((char*) &((item)->data) + (item)->nkey + 1 \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_data(item) ((char*) &((item)->data) + (item)->nkey + 1 \
         + (item)->nsuffix \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_ntotal(item) (sizeof(struct _stritem) + (item)->nkey + 1 \
         + (item)->nsuffix + (item)->nbytes \
         +(((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_clsid(item) ((item)->slabs_clsid & ~(3<<6))
#define ITEM_lruid(item) ((item)->slabs_clsid & (3<<6))

#define STAT_KEY_LEN 128
#define STAT_VAL_LEN 128

/** Append a simple stat with a stat name, value format and value */
#define APPEND_STAT(name, fmt, val) \
            append_stat(name, add_stats, (conn *)c, fmt, val);

/* Append an indexed stat with a stat name(with format), value format and value.
 */
#define APPEND_NUM_FMT_STAT(name_fmt, num, name, fmt, val)     \
  klen = snprintf(key_str, STAT_KEY_LEN, name_fmt, num, name);  \
  vlen = snprintf(val_str, STAT_VAL_LEN, fmt, val);             \
  add_stats(key_str, klen, val_str, vlen, c);

/** Common APPEND_NUM_FMT_STAT format.  */
#define APPEND_NUM_STAT(num, name, fmt, val)    \
    APPEND_NUM_FMT_STAT("%d:%s", num, name, fmt, val)

/**
 * Callback for any function producing stats.
 *
 * @param key the stat's key
 * @param klen length of the key
 * @param val the stat's value in an ascii form 
 * @param vlen length of the value
 * @param cookie magic callback cookie
 */
typedef void (*ADD_STAT)(const char *key, const uint16_t klen,
                         const char *val, const uint32_t vlen,
                         const void *cookie);

enum pause_thread_types {
    PAUSE_WORKER_THREADS = 0,
    PAUSE_ALL_THREADS,
    RESUME_ALL_THREADS,
    RESUME_WORKER_THREADS
};

enum store_item_type {
    NOT_STORED=0, STORED, EXISTS, NOT_FOUND, TOO_LARGE, NO_MEMORY
};

enum delta_result_type {
    OK, NON_NUMERIC, EOM, DELTA_ITEM_NOT_FOUND, DELTA_ITEM_CAS_MISMATCH
};

/* Time relative to server start. Smaller than time_t an 64-bit systems.
 * TODO: Move to sub-header. needed in looger.h
 * typdef unsigned int rel_time_t.
 */

/**
 * Use X macros to avoid iterating over the stats fields during reset and
 * aggregation. No longer have to add new stats in 3+ places.
 */

#define SLAB_STATS_FIELDS \
    X(set_cmds) \
    X(get_hits) \
    X(touch_hits)  \
    X(delete_hits)  \
    X(cas_hits) \
    X(cas_badval)   \
    X(incr_hits)    \
    X(decr_hits)

/** Stats stored per slab (and per thread) */
struct slab_stats {
#define X(name) uint64_t    name;
  SLAB_STATS_FIELDS
#undef X
};

#define THREAD_STATS_FIELDS \
    X(get_cmds) \
    X(get_misses)   \
    X(get_expired)  \
    X(get_flushed)  \
    X(touch_cmds)   \
    X(touch_misses) \
    X(delete_misses)    \
    X(incr_misses)  \
    X(decr_misses)  \
    X(cas_misses)   \
    X(bytes_read)   \
    X(bytes_written)    \
    X(flush_cmds)   \
    X(conn_yields)  /* # of yields for connections (-R option)*/ \
    X(auth_cmds)    \
    X(auth_errors)  \
    X(idle_kicks)   /* idle connections killed */

#ifdef EXTSTORE
#define EXTSTORE_THREAD_STATS_FIELDS    \
    X(get_extstore) \
    X(recache_from_extstore)    \
    X(miss_from_extstore)   \
    X(badcrc_from_extstore)
#endif

/**
 * Stats stored per-thread
 */
struct thread_stats {
    pthread_mutex_t     mutex;
#define X(name) uint64_t    name;
    THREAD_STATS_FIELDS
#ifdef EXTSTORE
    EXTSTORE_THREAD_STATS_FIELDS
#endif
#undef X
    struct slab_stats slab_stats[MAX_NUMBER_OF_SLAB_CLASSES];
    uint64_t lru_hits[POWER_LARGEST];
};

/* 
 * Globals stats. Only resettable stats should go into this structure.
 */
struct stats {
    uint64_t    total_items;
    uint64_t    total_conns;
    uint64_t    rejected_conns;
    uint64_t    malloc_fails;
    uint64_t    listen_disabled_num;
    uint64_t    slabs_move;     // times slabs were moved around
    uint64_t    slab_reassign_rescues; // items rescued during slab move
    uint64_t    slab_reassign_evictions_nomem; // valid items lost during slab
                                                // move
    uint64_t    slab_reassign_inline_reclaim;   // valid items lost during slab
                                                // move
    uint64_t    slab_reassign_chunk_rescues;    // chunked-item chunks recovered
    uint64_t    slab_reassign_busy_items;       // valid temporarily unmovable
    uint64_t    slab_reassign_busy_deletes;     // refcounted items killed
    uint64_t    lru_crawler_starts; // Number of item crawlers kicked off
    uint64_t    lru_maintainer_juggles; // number of LRU bg pokes
    uint64_t    time_in_listen_disabled_us; // elapsed time in microseconds 
                                            // while server unable to process
                                            // new connections
    uint64_t    log_worker_dropped;     // logs dropped by worker threads
    uint64_t    log_worder_written;     // logs written by worker threads
    uint64_t    log_watcher_skipped;    // logs watchers missed
    uint64_t    log_watcher_sent;       // logs sent to watcher buffers
#ifdef EXTSTORE
    uint64_t    extstore_compact_lost;  // items lost because they were locked
    uint64_t    extstore_compact_rescues; // items re-written during compaction
    uint64_t    extstore_compact_skipped; // unhit times skipped during 
                                            // compaction
#endif
    struct  timeval maxconns_entered;   // last time maxonns entered
};

/**
 * GLobal "state" stats. Reflects state that shouldn't be wiped ever.
 * Ordered for some cache line locality for commonly updated counters.
 */
struct stats_state {
    uint64_t        curr_items;
    uint64_t        curr_bytes;
    uint64_t        curr_conns;
    uint64_t        hash_bytes;     // size used for hash tables
    unsigned int    conn_structs;
    unsigned int    reserved_fds;
    unsigned int    hash_power_level; // Better hope it's not over 9000
    bool            hash_is_expanding; // If the hash table is being expanded
    bool            accepting_conns;    // whether we are currently accepting
    bool            slab_reassign_running; // slab reassign in progress
    bool            lru_crawler_running;    // crawl in progress
};

#define MAX_VERBOSITY_LEVEL 2

/**
 * When adding a setting, be sure to update process_stat_settings
 * Globally accessible settings as derived from commandline. 
 */
struct settings {
    size_t maxbytes;
    int verbose;
    rel_time_t oldest_live; // ignore existing items older than this
    uint64_t oldest_cas; // ignore existing items with CAS values lower than this
    double factor; // chunk size growth factor
    int chunk_size;
    int num_threads;
    bool use_cas;

    int item_size_max; // Maximum item size
    int slab_chunk_size_max; // Upper end for chunks within slab pages
    int slab_page_size; // Slab's page units

    bool lru_maintainer_thread; // LRU maintainer background thread
    bool slab_reassign; // Whether or not slab reassignment is allowed
    int slab_automove; // Whether or not to automatically move slabs
    double slab_automove_ratio;
    unsigned int slab_automove_window;
    int hashpower_init;
    int tail_repair_time;
    bool flush_enabled;
    char *hash_algorithm;

    bool lru_segmented; // Use split for flat LRU's
    bool inline_ascii_response; // pre-format the VALUE line for ASCII responses
    bool temp_lru;  // TTL < temporary_ttl uses TEMP_LRU
    uint32_t temporary_ttl; // temporary LRU threshold
#ifdef EXTSTORE
    unsigned int ext_item_size; // minimum size of items to store externally
    unsigned int ext_item_age;  // max age of tail item before storing ext.
    unsigned int ext_low_ttl;   // remaining TTL below this uses own pages
    unsigned int ext_recache_rate; // counter++ % recache_rate == 0 > recache
    unsigned int ext_wbuf_size; // read only note for the engine
    unsigned int ext_compact_under; // when fewer than this many pages, compact
    unsigned int ext_drop_under; // when fewer than this many pages, drop CLOD items
    double ext_max_frag;    // ideal maximum page fragmentation
    double slab_automove_freeratio; // % of memory to hold free as buffer
    bool ext_drop_unread;   // skip unread items during compaction
    // pre-slab-class free chunk limit
    unsigned int ext_free_memchunks[MAX_NUMBER_OF_SLAB_CLASSES];
#endif
};

extern struct settings settings;

#define ITEM_LINKED 1
#define ITEM_CAS 2

/* temp */
#define ITEM_SLABBED  4


/* Item was fetched at least once in its lifetime */
#define ITEM_FETCHED 8
/* Appended on fetch, removed on LRU shuffling */
#define ITEM_ACTIVE 16
/* If an item's storage are chained chunks */
#define ITEM_CHUNKED 32
#define ITEM_CHUNK 64
#ifdef EXTSTORE
/* ITEM_data bulk is external to item */
#define ITEM_HDR  128
#endif

/**
 * Structure for storing items within memcached.
 */
typedef struct _stritem {
    /* Protected by LRU locks*/
    struct _stritem     *next;
    struct _stritem     *prev;
    /* Rest are protected by an item lock */
    struct _stritem     *h_next;    // hash chain next
    rel_time_t          time;       // least recent access
    rel_time_t          exptime;    // expire time
    int                 nbytes;     // size of data
    unsigned short      refcount;   
    uint8_t             nsuffix;    // length of flags-and-length string
    uint8_t             it_flags;   // ITEM_* above
    uint8_t             slabs_clsid; // which slab class we're in
    uint8_t             nkey;       // key length, w/terminating null and padding
    /**
     * this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not using CAS.
     */
    union {
        uint64_t cas;
        char end;
    } data[];
    /* if it_flags & ITEM_CAS we have 8 bytes CAS
     * then null-terminated key
     * then " flags length\r\n" (to terminating null)
     * then data with terminating \r\m (no terminating null; it's binary!)
     */
} item;

/* Header when an item is actually a chunk of another item.*/
typedef struct _strchunk {
    struct _strchunk *next; // points within its own chain
    struct _strchunk *prev; // can potentially point to the head
    struct _stritem *head; // always points to the owner chunk
    int size;   // available chunk space in bytes
    int used;   // chunk space used
    int nbytes; // used.
    unsigned short refcount; // used?
    uint8_t orig_clsid; // for obj hdr chunks slabs_clsid is fake
    uint8_t it_flags; // ITEM_* above
    uint8_t slabs_clsid;    // Same as above
    char data[];
} item_chunk;
#ifdef EXSTORE
typedef struct {
    unsigned int page_version;  // from IO header
    unsigned int offset;        // from IO header
    unsigned short page_id;     // from IO header
} item_hdr;
#endif
typedef struct {
    pthread_t   thread_id;          // unique ID of this thread
    //struct event_base *base;      // libevent handle this thread uses
    //struct event notify_event;    // listen event for notify pipe
    int notify_receive_fd;          // receiving end of notify pip
    int notify_send_fd;             // sending end of notify pipe
    struct thread_stats stats;      // stats generated by this thread
    //struct conn_queue *new_conn_queue;  // queue of new connections to handle
    //cache_t *suffix_cache;          // suffix cache
#ifdef EXTSTORE
    //cache_t *io_cache;              // IO objects
    void *storage;                  // data object for stoage system
#endif
    //logger *l;                      // logger buffer
    void *lru_bump_buf;             // async LRU bump buffer
} LIBEVENT_THREAD;
#ifdef EXTSTORE
typedef struct _io_wrap {
    //obj_io  io;
    //struct _io_wrap *next;
    conn *c;
    item *hdr_it;                   // original header item.
    unsigned int iovec_start;       // start of the iovecs for this IO
    unsigned int iovec_count;       // total number of iovecs
    unsigned int iovec_data;        // specific index of data iovec
    bool miss;                      // signal a miss to unlink hdr_it
    bool badcrc;                    // signal a crc failure
    bool active;                    // FIXME: canary for test. remove
} io_wrap;
#endif
typedef struct conn conn;
/**
 * The structure representing a connection into memecached.
 */
struct conn {
    LIBEVENT_THREAD *thread; // Pointer to the thread object serving this 
                             // connection
};

/* current time of day (updated periodically)*/
extern volatile rel_time_t current_time;

/* TODO: Move to slabs.h? */
extern volatile int slab_rebalance_signal;

struct slab_rebalance {
    void *slab_start;
    void *slab_end;
    void *slab_pos;
    int s_clsid;
    int d_clsid;
    uint32_t busy_items;
    uint32_t rescues;
    uint32_t evictions_nomem;
    uint32_t inline_reclaim;
    uint32_t chunk_rescues;
    uint32_t busy_deletes;
    uint32_t busy_loops;
    uint8_t done;
};

extern struct slab_rebalance slab_rebal;

#define mutex_lock(x) pthread_mutex_lock(x)
#define mutex_unlock(x) pthread_mutex_unlock(x)

#include "slabs.h"
#include "assoc.h"
#include "items.h"
#include "hash.h"

/*
 * Functions such as the libevent-related calls that need to do cross-thread
 * communication in multithreaded mode (rather than actually doing the work
 * in the current thread) are called via "dispatch_" frontends, which are
 * also #define-d to directly call the underlying code in singlethreaded mode.
 */
void memcached_thread_init(int nthreads, void *arg);

item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes);
#define DO_UPDATE true
#define DONT_UPDATE false
item *item_get(const char *key, const size_t nkey, conn *c, const bool do_update);
item *item_touch(const char *key, const size_t nkey, uint32_t exptime, conn *c);
int item_link(item *it);
void item_remove(item *it);
int item_replace(item *it, item *new_it, const uint32_t hv);
void item_unlink(item *it);

void item_lock(uint32_t hv);
void *item_trylock(uint32_t hv);
void item_trylock_unlock(void *arg);
void item_unlock(uint32_t hv);
void pause_thread(enum pause_thread_types type);
#define refcount_incr(it) ++(it->refcount)
#define refcount_decr(it) --(it->refcount)
void STATS_LOCK(void);
void STATS_UNLOCK(void);

/* Stat processing functions */
void append_stat(const char *name, ADD_STAT add_stats, conn *c,
    const char *fmt, ...);
