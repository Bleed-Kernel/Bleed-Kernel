#pragma once

#define POLLIN      0x0001  // theres something to read
#define POLLPRI     0x0002  // an offscreen tty has new pixels to grab
#define POLLOUT     0x0004  // a write wont have to wait
#define POLLERR     0x0008
#define POLLHUP     0x0010  // the other end is gone
#define POLLNVAL    0x0020  // not an open fd

// not a real fd, stands for the callers own ipc queue
#define POLL_FD_IPC (-2)

#define POLL_MAX_FDS   MAX_FDS     // one for everything a task can have open, and room for the ipc queue
#define POLL_STACK_FDS 16          // more than this and the copy of them comes off the heap

typedef struct pollfd {
    int   fd;
    short events;
    short revents;
} pollfd_t;
