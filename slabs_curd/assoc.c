#include "assoc.h"

item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {
    item *it;
    unsigned int oldbucket;

    if (expanding && 
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket) {
        it = old_hashtable[oldbucket];
    } else {
        it = primary_hashtable[hv & hashmask(hashpower)];
    }

    item *ret = NULL;
    int depth = 0;
    while (it) {
        if ((nkey == it->nkey) && (memcmp(key, ITEM_key(it), nkey) == 0)) {
            ret = it;
            break;
        }
        it = it->h_next;
        ++depth;
    }
    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return ret;
}

/* Note: this isn't an assoc_update. The key must not already exist to call this*/
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;
    // assert(assoc_find(ITEM_key(it), it->nkey) == 0);
    // Shouldn't have duplicately named things defined
    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket) {
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;
    } else {
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey);
    return 1;
}

/* returns the address of the item pointer before the key. if *item == 0,
 * the item wasn't found.
 */
static item** _hashitem_before(const char *key, const size_t nkey, const uint32_t hv) {
    item **pos;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket) {
        pos = &old_hashtable[oldbucket];
    } else {
        pos = &primary_hashtable[hv & hashmask(hashpower)];
    }

    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {
        pos = &(*pos)->h_next;
    }
    return pos;
}

void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    item **before = _hashitem_before(key, nkey, hv);

    if (*before) {
        item *nxt;
        /* The DTrace probe cannot be triggered as the last instruction
         * due to possible tail-optimization by the compiler
         */
        MEMCACHED_ASSOC_DELETE(key, nkey);
        nxt = (*before)->h_next;
        (*before)->h_next = 0;  // probably pointless, but whatever
        *before = nxt;
        return;
    }
    /* Note: we never actually get here. the callers don't delete things
     * they can't find.
     */
    assert(*before != 0);
}
