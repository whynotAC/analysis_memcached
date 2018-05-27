#include "memcached.h"
#include "trace.h"

typedef struct {
    unsigned int size;      // sizes of items(item区域的大小)
    unsigned int perslab;   // how many items per slab(chunk中item的个数多少)

    void *slots;            // list of item ptrs(用于链接空闲的item)
    unsigned int sl_curr;   // total free items in list(用于记录空闲item的个数多少)

    unsigned int slabs;     // how many slabs were allocated for this class(用于记录分配了多少个slab或者叫做chunk)

    void **slab_list;       // array of slab pointers(用于记录每个chunk/slab的位置信息)
    unsigned int list_size; // size of prev array(slab_list的大小,当slabs == list_size时，需要进行扩容)

    size_t requested;       // The number of requested bytes(slab使用的字节数)
} slabclass_t;

static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES]; // 整个slab内存管理的结构体
static size_t mem_limit = 0;        // 可分配的内存大小
static size_t mem_malloced = 0;     // 已经分配用于使用的内存大小
/*
 * If the memory limit has been hit once. Used as a hint to decide when to
 * early-wake the LRU maintenance thread
 */
static bool mem_limit_reached = false;
static int power_largest;           // 最大slab的id号码

static void *mem_base = NULL;       // 分配内存的基地址指针
static void *mem_current = NULL;    // 现使用到的内存地址指针
static size_t mem_avail = 0;        // 可用内存大小
#ifdef EXTSTORE
static void *storage = NULL;
#endif

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
 * if maxslabs is 18(POWER_LARGEST - POWER_SMALLEST + 1), then all 
 * slab types can be made. if max memory is less than 18MB, only the
 * smaller ones can be made.
 */
static void slabs_preallocate(const unsigned int maxslabs);

// other
static void *get_page_from_global_pool(void);
static void split_slab_page_into_freelist(char *ptr, const unsigned int id);
static void do_slabs_free_chunked(item *it, const size_t size);
/*
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes) {
    int i = POWER_SMALLEST - 1;
    // 计算最小item的所占用的字节数
    // sizeof(item) = 48 (64位mac)
    // settings.chunk_size的默认大小也为48
    unsigned int size = sizeof(item) + settings.chunk_size;

    // 申请的内存大小，默认为64M
    mem_limit = limit;

    // 是否进行预申请内存
    if (prealloc) {
        // Allocate everything in a big chunk with malloc
        mem_base = malloc(mem_limit);
        if (mem_base != NULL) {
            printf("malloc size is %9zu\n", limit);
            mem_current = mem_base;
            mem_avail = mem_limit;
        } else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                            " one large chunk.\nWill allocate in smaller chunks\n");
        }
    }
    
    // 初始化slabclass数组置空
    memset(slabclass, 0, sizeof(slabclass));

    // 按照size*factor或者slab_sizes中数组的大小来决定slabclass每个id的大小
    // 保证不能超过MAX_NUMBER_OF_SLAB_CLASSES - 1 &&
    // 保证size*factor不能大于settings.slab_chunk_size_max的大小
    while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1) {
        if (slab_sizes != NULL) {
            if (slab_sizes[i-1] == 0) {
                break;
            }
            size = slab_sizes[i-1];
        } else if (size >= settings.slab_chunk_size_max / factor){
             break;
        }
        // Make sure items are always n-byte aligned
        // 保证8字节对齐
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }
        
        // 每个slabclass数组可存放item的大小
        slabclass[i].size = size;
        // 每个slab/chunk下可以保存item个数
        slabclass[i].perslab = settings.slab_page_size / slabclass[i].size;
        printf("index: %3d, size: %9u, perslab: %7u\n", i, slabclass[i].size, slabclass[i].perslab);
        if (slab_sizes == NULL) {
            size *= factor;
        }
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n", i, slabclass[i].size, slabclass[i].perslab);
        }
    }

    // 对slabclass最后一个slab进行分配
    power_largest = i;
    slabclass[power_largest].size = settings.slab_chunk_size_max;
    slabclass[power_largest].perslab = settings.slab_page_size / settings.slab_chunk_size_max;
    if (settings.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n", i, slabclass[i].size, slabclass[i].perslab);
    }

    // 对预申请的内存进行处理
    if (prealloc) {
        slabs_preallocate(power_largest);
    }
}

static void slabs_preallocate(const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /*
     * pre-allocate a 1MB slab in every size class so people don't get confused
     * by non-intuitive "SERVER_ERROR out of memory" messages. this is the most 
     * common question on the mailing list. if you really don't want this, you
     * can rebuild without these three lines.
     */
    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        if (++prealloc > maxslabs) {
            return;
        }
        // 对每个slabclass的内存进行划分
        if (do_slabs_newslab(i) == 0) {
            fprintf(stderr, "Error while preallocating slab memory!\n"
                            "If using -L or other prealloc options, max memory must be "
                            "at least %d megabytes.\n", power_largest);
            exit(1);
        }
    }
}

static int do_slabs_newslab(const unsigned int id) {
    slabclass_t *p = &slabclass[id];        // 根据索引取出slabclass的位置
    slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];

    // 获取待申请chunk大小，理论上每个chunk <= 1M
    // 但是有些情况 size * perslab不会正好等于1M而是小于1M
    // 有时不想字节浪费，就把settings.slab_reassign设置为true
    int len = (settings.slab_reassign || settings.slab_chunk_size_max != settings.slab_page_size)
              ? settings.slab_page_size
              : p->size * p->perslab;
    char *ptr;
    
    // 判断内存使用情况是否超过最大设定
    if ((mem_limit && mem_malloced + len > mem_limit && p->slabs > 0 && g->slabs == 0)) {
        mem_limit_reached = true;
        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }
    
    // grow_slab_list申请chunk/slab指针数组, 就是void **slab_list、list_size
    // get_page_from_global_pool从slabclass数组SLAB_GLOBAL_PAGE_POOL位置查看是否有空闲空间
    // memory_allocate从预申请的内存中划分出一个slab/chunk(1M空间)
    if ((grow_slab_list(id) == 0) ||
        (((ptr = (char*)get_page_from_global_pool()) == NULL) &&
         ((ptr = (char*)memory_allocate((size_t)len)) == 0))) {
        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    // 初始化slab/chunk中iem的指针域等信息
    memset(ptr, 0, (size_t)len);
    split_slab_page_into_freelist(ptr, id);

    // 保存当前chunk/slab的指针,并更新slabs的大小(slabs用于记录chunk使用了多少)
    p->slab_list[p->slabs++] = ptr;
    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

    return 1;
}

static int grow_slab_list(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    // 判断当前chunk/slab指针是否已经满了，当slabs等于list_size时,chunk满
    // 当满时就需要扩容
    if (p->slabs == p->list_size) {
        // 默认slab_list的大小为16
        // 当扩展时，扩展值为原来的两倍
        size_t new_size = (p->list_size != 0)? p->list_size * 2 : 16;
        void *new_list = realloc(p->slab_list, new_size * sizeof(void*));
        if (new_list == 0) return 0;
        // chunk/slab的数量多少
        p->list_size = new_size;
        // 只向chunk/slab地址的数组
        p->slab_list = (void **)new_list;
    }
    return 1;
}

// Fast FIFO queue
static void *get_page_from_global_pool(void) {
    // 从公共缓冲池里面分配空间
    slabclass_t *p = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    if (p->slabs < 1) {
        return NULL;
    }
    char *ret = (char*)(p->slab_list[p->slabs - 1]);
    p->slabs--;
    return ret;
}

static void *memory_allocate(size_t size) {
    void *ret;
    
    // 判断是否为预申请空间模式，如果不是则每次malloc申请1M
    if (mem_base == NULL) {
        // We are not using a preallocated large memory chunk
        ret = malloc(size);
    } else {
        // 当前内存使用位置
        ret = mem_current;
        
        // 申请空间不能大于可用的内存大小
        if (size > mem_avail) {
            return NULL;
        }

        // mem_current pointer _must_ be aligned!!!
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }
        
        // 更新现在的空闲空间的位置以及字节数
        mem_current = ((char*)mem_current) + size;
        if (size < mem_avail) {
            mem_avail -= size;
        } else {
            mem_avail = 0;
        }
    }
    // 已使用的内存大小
    mem_malloced += size;
    
    // 返回划分出来的内存位置
    return ret;
}

static void split_slab_page_into_freelist(char *ptr, const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    int x;
    // 当前chunk区域共有多少 perslab就是item
    for (x = 0; x < p->perslab; x++) {
        // 一个一个item进行划分
        do_slabs_free(ptr, 0, id);
        ptr += p->size;
    }
}

static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    // 把整个chunk/slab空间划分为一个一个item
    // 并将item使用slabclass的slot链接起来
    slabclass_t *p;
    item *it;

    assert(id >= POWER_SMALLEST && id <= power_largest);
    if (id < POWER_SMALLEST || id > power_largest)
        return;

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &slabclass[id];

    it = (item *)ptr;
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
#ifdef EXTSTORE
        bool is_hdr = it->it_flags & ITEM_HDR;
#endif
        it->it_flags = ITEM_SLABBED;
        it->slabs_clsid = 0;
        it->prev = 0;
        it->next = (item*)(p->slots);
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

static void do_slabs_free_chunked(item *it, const size_t size) {
    item_chunk *chunk = (item_chunk *)ITEM_data(it);
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
    it->next = (item*)(p->slots);
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
        chunk->next = (item_chunk*)p->slots;
        if (chunk->next) chunk->next->prev = chunk;
        p->slots = chunk;
        p->sl_curr++;
        p->requested -= chunk->size + sizeof(item_chunk);

        chunk = next_chunk;
    }

    return;
}


void slabs_init_show(void) {
    int i;
    slabclass_t *p = NULL;
    item *it = NULL;

    for (i = 0; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        printf("slabs index: %d\n", i);
        p = &slabclass[i];
        printf("slabs size: %3u, slabs preslab(chuk nubmer): %7u, item size: %7u, list size: %7u, requested: %7zu\n",
                p->size, p->perslab, p->sl_curr, p->list_size, p->requested);

        if (p->slots != NULL) {
            printf("item info: \n");
            it = (item*)(p->slots);
            printf("time: %7u, exptime: %7u, nbytes: %d, refcount:  %2u, nsuffix: %2u, it_flags: %2u, slabs_clsid: %2u, key: %2u\n",
                it->time, it->exptime, it->nbytes, it->refcount, it->nsuffix, it->it_flags, it->slabs_clsid, it->nkey);
        }
    }
}
