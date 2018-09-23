#include "bipbuffer.h"

#include <cstring>
#include <stdlib.h>

static size_t bipbuf_sizeof(const unsigned int size) {
    return sizeof(bipbuf_t) + size;
}

int bipbuf_unused(const bipbuf_t *me) {
    if (1 == me->b_inuse) {
        // distance between region B and region A
        return me->a_start - me->b_end;
    } else {
        return me->size - me->a_end;
    }
}

void bipbuf_init(bipbuf_t *me, const unsigned int size) {
    me->a_start = me->a_end = me->b_end = 0;
    me->size = size;
    me->b_inuse = 0;
}

bipbuf_t *bipbuf_new(const unsigned int size) {
    bipbuf_t *me = (bipbuf_t *)malloc(bipbuf_sizeof(size));
    if (!me)
        return NULL;
    bipbuf_init(me, size);
    return me;
}

static void __check_for_switch_to_b(bipbuf_t *me) {
    if (me->size - me->a_end < me->a_start - me->b_end)
        me->b_inuse = 1;
}

// TODO: DOCUMENT THESE TWO FUNCTIONS
unsigned char *bipbuf_request(bipbuf_t *me, const int size) {
    if (bipbuf_unused(me) < size)
        return 0;
    if (1 == me->b_inuse) {
        return (unsigned char *)me->data + me->b_end;
    } else {
        return (unsigned char *)me->data + me->a_end;
    }
}

int bipbuf_push(bipbuf_t *me, const int size) {
    if (bipbuf_unused(me) < size)
        return 0;

    if (1 == me->b_inuse) {
        me->b_end += size;
    } else {
        me->a_end += size;
    }

    __check_for_switch_to_b(me);
    return size;
}
