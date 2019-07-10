#ifndef EXTSTORE_H
#define EXTSTORE_H

// TODO: Temporary configuration structure. A "real" library should have an 
// extstore_set(enum, void *ptr) which hides the implementation.
// this is plenty for quick development.
struct extstore_conf {
    unsigned int page_size; // ideally 64-256M in size
    unsigned int page_count;
    unsigned int page_buckets;  // number of different writeable pages
    unsigned int wbuf_size; // must divide cleanly into page_size
    unsigned int wbuf_count; // this might get locked to "2 per active page"
    unsigned int io_threadcount;
    unsigned int io_depth;  // with normal I/O, hits locks less. req'd for AIO
};

#endif
