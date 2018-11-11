#pragma once

void *slab_automove_extstore_init(struct settings *settings);
void slab_automove_extstore_free(void *arg);
void slab_automove_extstore_run(void *arg, int *src, int *dst);
