#pragma once

typedef struct {
    unsigned long int size;

    // region A
    unsigned int a_start, a_end;

    // region B
    unsigned int b_end;

    // is B inuse?
    int b_inuse;

    unsigned char data[];
} bipbuf_t;

// TODO: DOCUMENTATION
unsigned char *bipbuf_request(bipbuf_t *me, const int size);
int bipbuf_push(bipbuf_t *me, const int size);

// @return bytes of unused space
int bipbuf_unused(const bipbuf_t *me);
