#include "self-test/ktests.h"

void kernel_self_test(){
    paging_test_self_test();
    pmm_test_self_test();
    pit_test_self_test();
    scheduler_test_self_test();
    vfs_test_self_test();
}