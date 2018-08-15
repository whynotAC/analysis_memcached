#ifndef MEMCACHED_H
#define MEMCACHED_H

/*
 *  Globally accessible settings as derived from the commandline.
 */
struct settings {
    
    bool use_cas;
    int slab_chunk_size_max; // Upper end for chunks within slab pages
    bool temp_lru;           // TTL < temporary_ttl uses TEMP_LRU
    uint32_t temporary_ttl;  // temporay LRU threshold
};

/* Slab sizing definitions.*/
#define POWER_SMALLEST 1
#define POWER_LARGEST 256   /* actual cap is 255 */
#define SLAB_GLOBAL_PAGE_POOL 0 /* magic slab class for storing for reassignment */
#define CHUNK_ALIGN_BYTES 8
/* slab class max is a 6-bit number, -1. */
#define MAX_NUMBER_OF_SLAB_CLASSES (63 + 1)

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

/*
 * The structure representing a connection into memcached.
 */
struct conn {
};

#define refcount_incr(it) ++(it->refcount)
#define refcount_decr(it) --(it->refcount)

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
