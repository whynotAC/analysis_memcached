/**
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
 */

/* powers-of-N allocation structures */

typedef struct {
    unsigned int size;      // sizes of items
    unsigned int perslab;   // how mamy items per slab

    void *slots;            // list of item ptrs
    unsigned int sl_curr;   // total free items in list

    unsigned int slabs;     // how many slabs were allocated for this class

    void **slab_list;       // array of slab pointers
    unsigned int list_size; // size of prev array

    size_t requested;       // The number of prev bytes
} slabclass_t;

static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
static size_t mem_limit = 0;
static size_t mem_malloced = 0;
/**
 * If the memory limit has been hit once. Used as a hint to decide when to 
 * early-wake the LRU maintenance thread
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
static pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Forward Declarations
 */
static int grow_slab_list(const unsigned int id);
static int do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);
static void do_slabs_free(void *ptr, const size_t size, unsigned int id);

/**
 * Preallocate as many slab pages as possible (called from slabs_init)
 * on start-up, so users don't get confused out-of-memory errors when
 * they do have free (in-slab) space, but no space to make new slabs.
 * if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
 * slab types can be made. if max memory is less than 18MB, only the 
 * smaller ones will be made.
 */
static void slabs_preallocate(const unsigned int maxslabs);
#ifdef EXTSTORE
void slabs_set_storage(void *arg) {
    storage = arg;
}
#endif
/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object.
 */
unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;

    if (size == 0 || size > settings.item_size_max)
        return 0;
    while (size > slabclass[res].size) {
        if (res++ == power_largest) {       // won't fit in the biggest slab
            return power_largest;
        }
    }
    return res;
}

/**
 * Determines the chunk size and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes) {
    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(item) + settings.chunk_size;

    mem_limit = limit;

    if (prealloc) {
        /* Allocate everything in a big chunk with malloc */
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

    while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1) {
        if (slab_sizes != NULL) {
            if (slab_sizes[i-1] == 0)
                break;
            size = slab_sizes[i-1];
        } else if (size >= settings.slab_chunk_size_max / factor) {
            break;
        }
        /* Make sure items are always n-byte aligned */
        if (size % CHUNK_ALIGN_BYTES)
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

        slabclass[i].size = size;
        slabclass[i].perslab = settings.slab_page_size / slabclass[i].size;
        if (slab_sizes == NULL)
            size *= factor;
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

    /* for the test suite: faking of how much we've already malloc'd */
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

void slabs_prefill_global(void) {
    void *ptr;
    slabclass_t *p = &slabclass[0];
    int len = settings.slab_page_size;

    while (mem_malloced < mem_limit 
            && (ptr = memory_allocate(len)) != NULL) {
        grow_slab_lit(0);
        p->slab_list[p->slab++] = ptr;
    }
    mem_limit_reached = true;
}

static void slabs_prellocate(const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /**
     * pre-allocate a 1MB slab in every size class so people don't get
     * confused by non-intuitive "SERVER_ERROR out of memory"
     * messages. this is the most common question on the mailling
     * list. if you really don't want this, you can rebuild without 
     * these three lines.
     */
    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        if (++prealloc > maxslabs) 
            return;
        if (do_slabs_newslab(i) == 0) {
            fprintf(stderr, "Error while preallocating slab memory!\n"
                "If using -L or other prealloc options, max meory has must be "
                "at least %d megabytes.\n", power_largest);
            exit(1);
        }
    }
}


