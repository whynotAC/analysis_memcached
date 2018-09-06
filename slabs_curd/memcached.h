#ifndef MEMCACHED_H
#define MEMCACHED_H

#define ENDIAN_BIG 1

#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "logger.h"

/* Slab sizing definitions.*/
#define POWER_SMALLEST 1
#define POWER_LARGEST 256   /* actual cap is 255 */
#define SLAB_GLOBAL_PAGE_POOL 0 /* magic slab class for storing for reassignment */
#define CHUNK_ALIGN_BYTES 8
/* slab class max is a 6-bit number, -1. */
#define MAX_NUMBER_OF_SLAB_CLASSES (63 + 1)

/**
 * How long an object can reasonably be assumed to be locked before
 * harvesting it on a low memory condition. Default: disabled
 */
#define TAIL_REPAIR_TIME_DEFAULT 0

// warning: don't use these macros with a function, as it evals its arg twice
#define ITEM_get_cas(item) (((item)->it_flags & ITEM_CAS) ? \
        (item)->data->cas : (uint64_t)0)

#define ITEM_set_cas(item,v) { \
    if ((item)->it_flags & ITEM_CAS) { \
        (item)->data->cas = v; \
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
        + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_clsid(item) ((item)->slabs_clsid & ~(3<<6))
#define ITEM_lruid(item) ((item)->slabs_clsid & (3<<6))

#define ITEM_LINKED 1
#define ITEM_CAS 2

// temp
#define ITEM_SLABBED 4

// Item was fetched at least once in its lifetime
#define ITEM_FETCHED 8
// Appended on fetch, removed on LRU shuffling
#define ITEM_ACTIVE 16
// If an item's storage are chained chunks
#define ITEM_CHUNKED 32
#define ITEM_CHUNK 64
#ifdef EXTSTORE
// ITEM_data bulk is external to item
#define ITEM_HDR 128
#endif

// Initial power multiplier for the hash table
#define HASHPOWER_DEFAULT 16
#define HASHPOWER_MAX 32

extern struct stats stats;
extern struct stats_state stats_state;
extern struct settings settings;

/**
 * Global stats. Only resettable stats should go into this structure.
 */
struct stats {
    uint64_t        total_items;
    uint64_t        total_conns;
    uint64_t        rejected_conns;
    uint64_t        malloc_fails;
    uint64_t        listen_disabled_num;
    uint64_t        slabs_moved;        // times slabs were moved around
    uint64_t        slab_reassign_rescues; // items rescued during slab move
    uint64_t        slab_reassign_evictions_nomem; // valid items lost during slab move
    uint64_t        slab_reassign_inline_reclaim; // valid items lost during slab move
    uint64_t        slab_reassign_chunk_rescues;  // chunked-item chunks recovered
    uint64_t        slab_reassign_busy_items;   // valid temporarily unmovable
    uint64_t        slab_reassign_busy_deletes; // refcounted items killed
    uint64_t        lru_crawler_starts; // Number of item crawlers kicked off
    uint64_t        lru_maintainer_juggles; // number of LRU bg pokes
    uint64_t        time_in_listen_disalbed_us; // elapsed time in microseconds while server unable to process new connections
    uint64_t        log_worker_dropped;     // logs dropped by worker threads
    uint64_t        log_worker_written;     // logs written by worker threads
    uint64_t        log_watcher_skipped;    // logs watchers missed
    uint64_t        log_watcher_sent;       // logs sent to watcher buffers
#ifdef EXTSTORE
    uint64_t        extstore_compact_lost;      // items lost becasue they were locked
    uint64_t        extstore_compact_rescues;   // items re-written during compaction
    uint64_t        extstore_compact_skipped;   // unhit items skipped during compaction
#endif
    struct timeval  maxconns_entered;       // last time maxconns entered
};

/**
 * Glocal "state" stats. Reflects state that shouldn't be wiped ever.
 * Ordered for some cache line locality for commonly updated counters.
 */
struct stats_state {
    uint64_t        curr_items;
    uint64_t        curr_bytes;
    uint64_t        curr_conns;
    uint64_t        hash_bytes; // size used for hash tables
    unsigned int    conn_structs;
    unsigned int    reserved_fds;
    unsigned int    hash_power_level;   // Better hope it's not over 9000
    bool            hash_is_expanding;  // If the hash table is being expanded
    bool            accepting_conns;    // whether we are currently accepting
    bool            slab_reassign_running; // slab reassign in progress
    bool            lru_crawler_running;    // crawl in progress
};

/*
 *  Globally accessible settings as derived from the commandline.
 */
struct settings {
    size_t maxbytes;        // memcached所占用的空间大小
    int verbose;            // 观察器，数值越大，查看的信息越多
    double factor;          // chunk size growth factor(item的成长因子)
    int chunk_size;         // 最小的item中数据的大小
    
    bool use_cas;
    int slab_chunk_size_max;    // Upper end for chunks within slab pages
    int slab_page_size;         // Slab's page units.(划分内存空间的单位大小，默认为1M)
    bool lru_segmented;         // Use split or flat LRU's (是否使用lru分片)
    bool slab_reassign;         // Whether or not slab reassignment is allowed
                                // (是否按照slab_page_size进行页面分配，以便后面进行页面automoved)
    char *hash_algorithm;       // Hash algorithm in use(Hash表使用的Hash算法)
    int hot_lru_pct;            // percentage of slab space for HOT_LRU (HOT_LRU链所占用的空间大小比例)
    int warm_lru_pct;           // percentage of slab space for WARM_LRU (WARM_LRU链所占用的空间大小比例)
    int hashpower_init;         // Starting hash power level(Hash表的大小)
    
    int tail_repair_time;       // LRU tail refcount leak repair time (将worker线程使用的item强制释放的时间)

    bool inline_ascii_response; // pre-format the VALUE line for ASCII response (将value转变成ascii字符串)
    bool temp_lru;              // TTL < temporary_ttl uses TEMP_LR(是否启用TEMP_LRU链表)
    uint32_t temporary_ttl;     // temporay LRU threshold (item加入TEMP_LRU的时间戳)

    int num_threads;            // number of worker(without dispatcher libevent threads to run)
                                // 工作线程数，由这个数字来决定hash表锁的大小
};


/**
 *  Structure for storing items within memcached.
 */
typedef struct _stritem {
    /* Protected by LRU locks */
    struct _stritem *next;
    struct _stritem *prev;
    /* Rest are protected by an item lock */
    struct _stritem *h_next;    // hash chain next
    rel_time_t      time;       // least recent access
    rel_time_t      exptime;    // expire time
    int             nbytes;     // size of data
    unsigned short  refcount;
    uint8_t         nsuffix;    // length of flags-and-length string
    uint8_t         it_flags;   // ITEM_* above
    uint8_t         slabs_clsid;// which slab class we're in
    uint8_t         nkey;       // key length, w/terminating null and padding
    /*
     *  This odd type prevents type-punning issues when we do
     *  the little shuffle to save space when not using CAS.
     */
    union {
        uint64_t cas;
        char     end;
    } data[];
    /*
     *  if it_flags & ITEM_CAS we have 8 bytes CAS
     *  then null-terminated key
     *  then " flags length\r\n" (no terminating null)
     *  then data with terminating \r\n (no terminating null; it's binary!)
     */
} item;

// Header when an item is octually a chunk of another item
typedef struct _strchunk {
    struct _strchunk *next;         // points within its own chain
    struct _strchunk *prev;         // can potentially point to the head
    struct _strchunk *head;         // alawys points to the owner chunk
    int               size;         // avaliable chunk space in bytes
    int               used;         // chunk space used
    int               nbytes;       // uesd
    unsigned short    refcount;     // used?
    uint8_t           orig_clsid;   // for obj hdr chunks slabs_clsid is fake
    uint8_t           it_flags;     // ITEM_* above
    uint8_t           slabs_clsid;  // Same as above
    char              data[];
} item_chunk;
#ifdef EXTSTORE
typedef struct {
    unsigned int page_version;      // from IO header
    unsigned int offset;            // from IO header
    unsigned short page_id;         // from IO header
} item_hdr;
#endif

enum pause_thread_types {
    PAUSE_WORKER_THREADS = 0,
    PAUSE_ALL_THREADS,
    RESUME_ALL_THREADS,
    RESUME_WORKER_THREADS
};

/*
 * The structure representing a connection into memcached.
 */
struct conn {
};

#define mutex_lock(x) pthread_mutex_lock(x)
#define mutex_unlock(x) pthread_mutex_unlock(x)

void memcached_thread_init(int nthreads, void *arg);

void item_lock(uint32_t hv);
void *item_trylock(uint32_t hv);
void item_trylock_unlock(void *arg);
void item_unlock(uint32_t hv);
void pause_threads(enum pause_thread_types type);


#define refcount_incr(it) ++(it->refcount)
#define refcount_decr(it) --(it->refcount)
void STATS_LOCK(void);
void STATS_UNLOCK(void);

enum store_item_type {
    NOT_STORED=0, STORED, EXISTS, NOT_FOUND, TOO_LARGE, NOT_MEMORY
};

enum delta_result_type {
    OK, NON_NUMERIC, EOM, DELTA_ITEM_NOT_FOUND, DELTA_ITEM_CAS_MISMATCH
};

enum delta_result_type do_add_delta(conn *c, const char *key,
                                    const size_t nkey, const bool incr,
                                    const int64_t delta, char *buf,
                                    uint64_t *cas, const uint32_t hv);
enum store_item_type do_store_item(item *item, int comm, conn *c, const uint32_t hv);

#endif
