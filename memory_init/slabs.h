#ifndef SLABS_H
#define SLABS_H

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
using namespace std;
/*
 * Init the subsystem. 
 * 1st argument is the limit on no. of bytes to allocate, 0 if no limit.
 * 2nd argument is the growth factor; each slab will use a chunk size equal to
 * the previous slab's chunk size times this factor.
 * 3rd argument specifies if the slab allocator should allocate all memory up
 * front (if ture),or allocate memory in chunks as it is needed (if false).
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes);


// use show slabs info
void slabs_init_show(void);

#endif
