#pragma once

// never return, break or goto out of the block, clac only runs when it falls off the end
#define SMAP_ALLOW for (int _i = (stac(), 0); !_i; clac(), _i++)

int SMAP_init(void);

extern int smap_supported;

static inline void stac(void){
    if (!smap_supported) return;
    asm volatile("stac" ::: "cc");
}

static inline void clac(void){
    if (!smap_supported) return;
    asm volatile("clac" ::: "cc");
}