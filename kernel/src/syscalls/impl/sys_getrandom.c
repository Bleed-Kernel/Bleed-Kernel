#include <random/random.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <string.h>

#define GETRANDOM_MAX   33554431

long sys_getrandom(uint64_t user_buf, uint64_t len, uint64_t flags) {
    if (flags & ~(uint64_t)(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
        return -EINVAL;
    if ((flags & GRND_RANDOM) && (flags & GRND_INSECURE))
        return -EINVAL;
    if (len > GETRANDOM_MAX)
        len = GETRANDOM_MAX;
    if (!len)
        return 0;

    task_t  *task = get_current_task();
    uint8_t  chunk[256];
    uint64_t done = 0;

    while (done < len) {
        size_t n = len - done < sizeof(chunk) ? (size_t)(len - done) : sizeof(chunk);
        random_get_bytes(chunk, n);
        if (copy_to_user(task, (void *)(user_buf + done), chunk, n) != 0) {
            memset(chunk, 0, sizeof(chunk));
            return done ? (long)done : -EFAULT;
        }
        done += n;
    }
    memset(chunk, 0, sizeof(chunk));
    return (long)done;
}
