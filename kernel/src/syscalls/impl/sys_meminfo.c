#include <boot/sysinfo/sysinfo.h>
#include <user/user_copy.h>
#include <mm/kalloc.h>

int sys_meminfo(system_memory_info_t *user_buf){
    if (!user_buf) return -1;
    system_memory_info_t *info = get_system_memory_info();
    copy_to_user(user_buf, info, sizeof(system_memory_info_t));
    kfree(info, sizeof(system_memory_info_t));
    return 0;
}