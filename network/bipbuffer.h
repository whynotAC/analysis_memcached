#ifndef BIPBUFFER_H
#define BIPBUFFER_H

typedef struct {
    
    unsigned long int size;

    /* region A */
    unsigned int a_start, a_end;

    /* region B */
    unsigned int b_end;

    /* is B inuse? */
    int b_inuse;

    unsigned char data[];
} bipbuf_t;

/**
 * Create a new bip buffer
 *
 * malloc()s spare
 *
 * @param[in] size The size of the buffer
 */
bipbuf_t *bipbuf_new(const unsigned int size);

/**
 * Initialise a bip buffer. Use memory provided by user.
 *
 * No malloc() are performed.
 *
 * @param[in] size The size of the array.
 */
void bipbuf_init(bipbuf_t *me, const unsigned int size);

#endif
