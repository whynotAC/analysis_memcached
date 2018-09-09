#include "bipbuffer.h"

int bipbuf_unused(const bipbuf_t *me) {
    if (1 == me->b_inuse) {
        // distance between region B and region A
        return me->a_start - me->b_end;
    } else {
        return me->size - me->a_end;
    }
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
