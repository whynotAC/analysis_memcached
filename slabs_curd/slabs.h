#pragma once

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
