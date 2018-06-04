#ifndef MEMCACHED
#define MEMCACHED

#define ENDIAN_BIG 1

#include "logger.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>

// Slab sizing definitions.
#define POWER_SMALLEST 1
#define POWER_LARGEST 256 // actual cap is 255
#define SLAB_GLOBAL_PAGE_POOL 0 // magic slab class for storing pages for reassignment
#define CHUNK_ALIGN_BYTES 8
// slab class max is a 6-bit number, -1.
#define MAX_NUMBER_OF_SLAB_CLASSES (63 + 1)

#define ITEM_key(item) (((char*)&((item)->data)) \
        + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_data(item) ((char*) &((item)->data) + (item)->nkey + 1 \
         + (item)->nsuffix \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_CAS 2

// temp
#define ITEM_SLABBED 4

// if an item's storage are chained chunks
#define ITEM_CHUNKED 32
#define ITEM_CHUNK   64
#ifdef EXTSTORE
// ITEM_data bulk is external to item
#define ITEM_HDR 128
#endif

/* Initial power multiplier for the hash table */
#define HASHPOWER_DEFAULT 16
#define HASHPOWER_MAX 32

extern struct settings settings;
/*
 * When adding a setting, be sure to update process_stat_settings
 *
 * Globally accessible settings as derived from the commandline.
 */
struct settings {
    size_t maxbytes;            // memcached所占用的空间大小
    int verbose;                // 查看内存的分配情况，可将此值设置为2
    double factor;  // chunk size growth factor(item的成长因子)
    int chunk_size;             // 最小的item中数据的大小
    
    bool use_cas;
    int slab_chunk_size_max; // Upper end for chunks within slab pages.(最大chunk的大小,默认为slab_page_size的一半)
    int slab_page_size;     // Slab's page units.(划分内存空间的单位大小，默认为1M)
    bool slab_reassign;     // Whether or not slab reassignment is allowed(是否对内存进行紧缩，减少浪费的空间)

    char *hash_algorithm;   // Hash algorithm in use(Hash表使用的Hash算法)
    int hashpower_init;     // Starting hash power level(Hash表的大小)

    int num_threads;        // number of worker (without dispatcher libevent threads to run(工作线程数，由这个数字来决定hash锁表的大小)
};

/*
 * Structure for storing items within memcached
 */
typedef struct _stritem {
    // Protected by LRU locks
    struct _stritem *next;
    struct _stritem *prev;
    // Rest are protected by an item lock
    struct _stritem *h_next; // hash chain next
    rel_time_t      time;         // least recent access
    rel_time_t      exptime;     // expire time
    int             nbytes;     // size of data
    unsigned short  refcount;   // 引用计数器，当线程操作该item时,refcount会加1
    uint8_t         nsuffix;    // length of flags-and-length string
    uint8_t         it_flags;   // ITEM_* above
    uint8_t         slabs_clsid; // which slab class we're in
    uint8_t         nkey;       // key length, w/terminating null and padding
    /*
     * this add type prevents type-punning issuses when we do 
     * the little shuffle to save space when not using CAS.
     */
    union {
        uint64_t cas;
        char end;
    } data[];
    /*
     * if it_flags & ITEM_CAS we have 8 bytes CAS
     * then null-terminated key
     * then " flags length\r\n" (no terminating null)
     * then data with terminating \r\n (no terminating null; it's binary!)
     */
} item;

// Header when an item is octually a chunk of another item
typedef struct _strchunk {
    struct _strchunk *next;         // points within its own chain
    struct _strchunk *prev;         // can potentially point to the head
    struct _strchunk *head;         // alawys points to the owner chunk
    int              size;          // avaliable chunk space in bytes
    int              used;          // chunk space used
    int              nbytes;        // used.
    unsigned short   refcount;      // used?
    uint8_t          orig_clsid;    // for obj hdr chunks slabs_clsid is fake
    uint8_t          it_flags;      // ITEM_* above
    uint8_t          slabs_clsid;   // Same as above
    char             data[];
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

#define mutex_lock(x) pthread_mutex_lock(x)
#define mutex_unlock(x) pthread_mutex_unlock(x)

void memcached_thread_init(int nthreads, void *arg);

void item_lock(uint32_t hv);
void *item_trylock(uint32_t hv);
void item_trylock_unlock(void *arg);
void item_unlock(uint32_t hv);
void pause_threads(enum pause_thread_types type);

#endif
