#pragma once

#include <string>

/**
 * Init the subsystem. 1st argument is  the limit on no. of bytes to allocate,
 * 0 if no limit. 2nd argument is the growth factor; each slab will use a chunk
 * size equal to the previous slab's chunk size times this factor.
 * 3rd argument specifies if the slab allocator should allocate all memory
 * up front (if ture), or allocate memory in chunks as it is needed (if false).
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes);

/**
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object.
 */
unsigned int slabs_clsid(const size_t size);

/** Allocate object of given length. 0 on error */
#define SLABS_ALLOC_NO_NEWPAGE 1
void *slabs_alloc(const size_t size, unsigned int id, uint64_t *total_bytes, unsigned int flags);

/** Free previously allocated object */
void slabs_free(void *ptr, size_t size, unsigned int id);

void slabs_mlock(void);
void slabs_munlock(void);

enum reassign_result_type {
    REASSIGN_OK=0, REASSIGN_RUNNING, REASSIGN_BADCLASS, REASSIGN_NOSPARE,
    REASSIGN_SRC_DST_SAME
};

enum reassign_result_type slabs_reassign(int src, int dst);

void displayslabs();
void displayMemory();
