#pragma once

#include <stdint.h>

// what stat hands back, linuxs x86_64 struct stat so musl built code reads it right
typedef struct user_stat {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t mode;
    int32_t  uid;
    int32_t  gid;
    uint32_t pad;
    uint64_t rdev;
    int64_t  size;
    int64_t  blksize;
    int64_t  blocks;
    int64_t  atime;
    int64_t  atime_nsec;
    int64_t  mtime;
    int64_t  mtime_nsec;
    int64_t  ctime;
    int64_t  ctime_nsec;
    int64_t  unused[3];
} user_stat_t;

#define STAT_IFMT   0170000
#define STAT_IFDIR  0040000
#define STAT_IFREG  0100000
#define STAT_IFCHR  0020000
#define STAT_IFIFO  0010000
#define STAT_IFSOCK 0140000
#define STAT_IFLNK  0120000
