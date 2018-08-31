#include "slabs.h"

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>

#include "memcached.h"

/*
 * #define DEBUG_SLAB_MOVER
 * powers-of-N allocation structures
 **/
typedef struct {
    unsigned int size;      // sizes of items
    unsigned int perslab;   // how many items per slab

    void *slots;            // list of item ptrs
    unsigned int sl_curr;   // total free items in list

    unsigned int slabs;     // how many slabs were allocated for this class

    void **slab_list;       // array of slab pointers.
    unsigned int list_size; // size of prev array

    size_t requested;       // The number of requested bytes.
} slabclass_t;

static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
static size_t mem_limit = 0;
static size_t mem_malloced = 0;
/*  If the memory limit has been hit once. Used as a hint to decide when to
 *  early-wake the LRU maintenance thread
 */
static bool mem_limit_reached = false;
static int power_largest;

static void *mem_base = NULL;
static void *mem_current = NULL;
static size_t mem_avail = 0;
#ifdef EXTSTORE
static void *storage = NULL;
#endif
/**
 * Access to the slab allocator is protected by this lock
 */
static pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Forward Declarations
 */
static int grow_slab_list(const unsigned int id);
static int do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);
static void do_slabs_free(void *ptr, const size_t size, unsigned int id);

/*
 * Preallocate as many slab pages as possible (called from slabs_init)
 * on start-up, so users don't get confused out-of-memory errors when
 * they do have free (in-slab) space, but no space to make new slabs.
 * if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
 * slab types can be made. if max memory is less than 18 MB, only the
 * smaller ones will be made.
 */
static void slabs_preallocate(const unsigned int maxslabs);
#ifdef EXTSTORE
void slabs_set_storage(void *arg) {
    storage = arg;
}
#endif

/**
 *  Figures out which slab class (chunk size) is required to store an item of
 *  a given size.
 *
 *  Given object size, return id to use when allocating/freeing memory for
 *  object
 *  0 mean error: can't store such a large object.
 */
unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;

    if (size == 0 || size > settings.item_size_max)
        return 0;
    while (size > slabclass[res].size)
        if (res++ == power_largest) /* won't fit in the biggest slab*/
            return power_largest;
    return res;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes) {
    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(item) + settings.chunk_size;

    mem_limit = limit;

    if (prealloc) {
        // Allocate everything in a big chunk with malloc
        mem_base = malloc(mem_limit);
        if (mem_base != NULL) {
            mem_current = mem_base;
            mem_avail = mem_limit;
        } else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                " on large chunk.\nWill allocate in smaller chunks\n");
        }
    }

    memset(slabclass, 0, sizeof(slabclass));

    while (++i < MAX_NUMBER_OF_SLAB_CLASSES - 1) {
        if (slab_sizes != NULL) {
            if (slab_size[i-1] == 0) {
                break;
            }
            size = slab_sizes[i-1];
        } else if (size >= settings.slab_chunk_size_max / factor) {
            break;
        }
        // Make sure items are always n-byte aligned
        if (size % CHUNK_ALIGN_BYTES)
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

        slabclass[i].size = size;
        slabclass[i].perslab = settings.slab_page_size / slabclass[i].size;
        if (slab_sizes == NULL) {
            size *= factor;
        }
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, slabclass[i].size, slabclass[i].perslab);
        }
    }
    power_largest = i;
    slabclass[power_largest].size = settings.slab_chunk_size_max;
    slabclass[power_largest].perslab = settings.slab_page_size / settings.slab_chunk_size_max;
    if (settings.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
            i, slabclass[i].size, slabclass[i].perslab);
    }
        // for the test suite: faking of how much we've already malloc'd
    {
        char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
        if (t_initial_malloc) {
            mem_malloced = (size_t)atol(t_initial_malloc);
        }
    }

    if (prealloc) {
        slabs_preallocate(power_largest);
    }
}

static void slabs_preallocate(const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /* pre-allocate a 1MB slab in every size class so people don't get
     * confused by non-intuitive "SERVER_ERROR out of memory"
     * messages. this is the most common question on the mailing
     * list. if you really don't want this, you can rebuild without
     * these three lines.
     */
    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        if (++prealloc > maxslabs)
            return;
        if (do_slabs_newslab(i) == 0) {
            fprintf(stderr, "Error while preallocating slab memory!\n"
                "If using -L or other prealloc options, max memory must be "
                "at least %d megabytes.\n", power_largest);
            exit(1);
        }
    }
}

void slabs_free(void *ptr, size_t size, unsigned int id) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_free(ptr, size, id);
    pthread_mutex_unlock(&slabs_lock);
}

static void do_slabs_free_chunked(item *it, const size_t size) {
    item_chunk *chunk = (item_chunk *) ITEM_data(it);
    slabclass_t *p;

    it->it_flags = ITEM_SLABBED;
    it->slabs_clsid = 0;
    it->prev = 0;
    // header object's original classid is stored in chunk
    p = &slabclass[chunk->orig_clsid];
    if (chunk->next) {
        chunk = chunk->next;
        chunk->prev = 0;
    } else {
        // header with no attached chunk
        chunk = NULL;
    }

    // return the header object
    // TODO: This is in three places, here and in do_slabs_free()
    it->prev = 0;
    it->next = p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it;
    p->sl_curr++;
    // TODO: macro
    p->requested -= it->nkey + 1 + it->nsuffix + sizeof(item) + sizeof(item_chunk);
    if (settings.use_cas) {
        p->requested -= sizeof(uint64_t);
    }

    item_chunk *next_chunk;
    while (chunk) {
        assert(chunk->it_flags == ITEM_CHUNK);
        chunk->it_flags = ITEM_SLABBED;
        p = &slabclass[chunk->slabs_clsid];
        chunk->slabs_clsid = 0;
        next_chunk = chunk->next;

        chunk->prev = 0;
        chunk->next = p->slots;
        if (chunk->next) chunk->next->prev = chunk;
        p->slots = chunk;
        p->sl_curr++;
        p->requested -= chunk->size + sizeof(item_chunk);

        chunk = next_chunk;
    }

    return;
}

static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    slabclass_t *p;
    item *it;

    assert(id >= POWER_SMALLEST && id <= power_largest);
    if (id < POWER_SMALLEST || id > power_largest)
        return;

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &slabclass[id];

    it = (item*)ptr;
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
#ifdef EXTSTORE
        bool is_hdr = it->it_flags & ITEM_HDR;
#endif
        it->it_flags = ITEM_SLABBED;
        it->slabs_clsid = 0;
        it->prev = 0;
        it->next = p->slots;
        if (it->next) it->next->prev = it;
        p->slots = it;

        p->sl_curr++;
#ifdef EXTSTORE
        if (!is_hdr) {
            p->requested -= size;
        } else {
            p->requested -= (size - it->nbytes) + sizeof(item_hdr);
        }
#else
        p->requested -= size;
#endif
    } else {
        do_slabs_free_chunked(it, size);
    }
    return;
}

void *slabs_alloc(size_t size, unsigned int id, uint64_t *total_bytes,
                    unsigned int flags) {
    void *ret;

    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_alloc(size, id, total_bytes, flags);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

static void *do_slabs_alloc(const size_t size, unsigned int id, uint64_t *total_bytes,
                            unsigned int flags) {
    slabclass_t *p;
    void *ret = NULL;
    item *it = NULL;

    if (id < POWER_SMALLEST || id > power_largest) {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, 0);
        return NULL;
    }
    p = &slabclass[id];
    assert(p->sl_curr == 0 || ((item *)p->slots)->slabs_clsid == 0);
    if (total_bytes != NULL) {
        *total_bytes = p->requested;
    }

    assert(size <= p->size);

    /*
     * fail unless we have space at the end of a recently allocated page,
     * we have something on our freelist, or we could allocate a new page
     */
    if (p->sl_curr == 0 && flags != SLABS_ALLOC_NO_NEWPAGE) {
        do_slabs_newslab(id);
    }

    if (p->sl_curr != 0) {
        /*
         * return off our freelist
         */
        it = (item *)p->slots;
        p->slots = it->next;
        if (it->next) it->next->prev = 0;
        /*
        * Kill flag and initialize refcount here for lock safety in slab
        * mover's freeness detection
        */
        it->it_flags &= ~ITEM_SLABBED;
        it->refcount = 1;
        p->sl_curr--;
        ret = (void*)it;
    } else {
        ret = NULL;
    }

    if (ret) {
        p->requested += size;
        MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
    }

    return ret;
}

static int do_slabs_newslab(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    int len = (settings.slabs_reassign || settings.slab_chunk_size_max != settings.slab_page_size)
              ? settings.slab_page_size : p->size * p->perslab;
    char *ptr;

    if ((mem_limit && mem_malloced + len > mem_limit && p->slabs > 0
         && g->slabs == 0)) {
        mem_limit_reached = true;
        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    if ((grow_slab_list(id) == 0) ||
        (((ptr = get_page_from_global_pool()) == NULL) && 
        ((ptr = memory_allocate((size_t)len)) == 0))) {
        
        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    memset(ptr, 0, (size_t)len);
    split_slab_page_into_freelist(ptr, id);

    p->slab_list[p->slabs++] = ptr;
    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

    return 1;
}

static int grow_slab_list(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    if (p->slabs == p->list_size) {
        size_t new_size = (p->list_size != 0) ? p->list_size * 2 : 16;
        void *new_list = realloc(p->slab_list, new_size * sizeof(void *));
        if (new_list == 0) return 0;
        p->list_size = new_size;
        p->slab_list = new_list;
    }
    return 1;
}

/*
 *  fast FIFO queue
 */
static void *get_page_from_global_pool(void) {
    slabclass_t *p = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    if (p->slabs < 1) {
        return NULL;
    }
    char *ret = p->slab_list[p->slabs - 1];
    p->slabs--;
    return ret;
}

static void *memory_allocate(size_t size) {
    void *ret;

    if (mem_base == NULL) {
        /* We are not using a preallocated large memory chunk */
        ret = malloc(size);
    } else {
        ret = mem_current;

        if (size > mem_avail) {
            return NULL;
        }

        /*
         * mem_current pointer _must_ be aligned!!!
         */
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }

        mem_current = ((char*)mem_current) + size;
        if (size < mem_avail) {
            mem_avail -= size;
        } else {
            mem_avail = 0;
        }
    }
    mem_malloced += size;

    return ret;
}

static void split_slab_page_into_freelist(char *ptr, const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    int x;
    for (x = 0; x < p->perslab; x++) {
        do_slabs_free(ptr, 0, id);
        ptr += p->size;
    }
}
