
#include "bipbuffer.h"

static size_t bipbuf_sizeof(const unsigned int size) {
    return sizeof(bipbuf_t) + size;
}

void bipbuf_init(bipbuf_t *me, const unsigned int size) {
    me->a_start = me->a_end = me->b_end = 0;
    me->size = size;
    me->b_inuse = 0;
}

bipbuf_t *bipbuf_new(const unsigned int size) {
    bipbuf_t *me = malloc(bipbuf_sizeof(size));
    if (!me)
        return NULL;
    bipbuf_init(me, size);
    return me;
}
