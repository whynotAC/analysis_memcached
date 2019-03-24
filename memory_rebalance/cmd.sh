#!/bin/sh

g++ memcached.c globals.c thread.c slabs.c slab_automove_extstore.c slab_automove.c murmur3_hash.c jenkins_hash.c items.c hash.c assoc.c -o memory_rebalance

