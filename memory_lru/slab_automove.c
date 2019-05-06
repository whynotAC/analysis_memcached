#include "slab_automove.h"
#include <stdlib.h>
#include <string.h>

#define MIN_PAGES_FOR_SOURCE 2
#define MIN_PAGES_FOR_RECLAIM 2.5

struct window_data {
    uint64_t age;
    uint64_t dirty;
    uint64_t evicted;
};

typedef struct {
    struct window_data *window_data;
    uint32_t window_size;
    uint32_t window_cur;
    double max_age_ratio;
    item_stats_automove iam_before[MAX_NUMBER_OF_SLAB_CLASSES]; // item before
    item_stats_automove iam_after[MAX_NUMBER_OF_SLAB_CLASSES]; // item after
    slab_stats_automove sam_before[MAX_NUMBER_OF_SLAB_CLASSES]; // slab before
    slab_stats_automove sam_after[MAX_NUMBER_OF_SLAB_CLASSES]; // slab after
} slab_automove;

void *slab_automove_init(struct settings *settings) {
    uint32_t window_size = settings->slab_automove_window;
    double max_age_ratio = settings->slab_automove_ratio;
    slab_automove *a = calloc(1, sizeof(slab_automove));
    if (a == NULL)
        return NULL;
    a->window_data = calloc(window_size * MAX_NUMBER_OF_SLAB_CLASSES, sizeof(struct window_data));
    a->window_size = window_size;
    a->max_age_ratio = max_age_ratio;
    if (a->window_data == NULL) {
        free(a);
        return NULL;
    }

    // do a dry run to fill the before structs
    fill_item_stats_automove(a->iam_before);
    fill_slab_stats_automove(a->sam_before);

    return (void *)a;
}

void slab_automove_free(void *arg) {
    slab_automove *a = (slab_automove *)arg;
    free(a->window_data);
    free(a);
}

static void window_sum(struct window_data *wd, struct window_data *w, uint32_t size) {
    int x;
    for (x = 0; x < size; x++) {
        struct window_data *d = &wd[x];
        w->age += d->age;
        w->dirty += d->dirty;
        w->evicted += d->evicted;
    }
}

// TODO: if oldest is dirty, find next oldest.
// still need to base ratio off of absolute age
void slab_automove_run(void *arg, int *src, int *dst) {
    
}
