#include "memcached.h"

volatile rel_time_t current_time;

// exported globals
struct stats stats;
struct stats_state stats_state;
struct settings settings;
