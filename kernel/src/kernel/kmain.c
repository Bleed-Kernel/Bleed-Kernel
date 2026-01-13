#include <stdint.h>
#include <stdio.h>
#include <ansii.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <string.h>
#include <mm/pmm.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <drivers/serial/serial.h>
#include <drivers/pic/pic.h>
#include <drivers/pit/pit.h>
#include <fs/vfs.h>
#include <status.h>
#include <fs/archive/tar.h>
#include <sys/sleep.h>
#include <fonts/psf.h>
#include <drivers/framebuffer/framebuffer.h>
#include <boot/sysinfo/sysinfo.h>
#include <devices/type/tty_device.h>
#include <devices/device_io.h>
#include <console/console.h>
#include <sched/scheduler.h>
#include <threads/exit.h>
#include <cpu/stack_trace.h>
#include <syscalls/syscall.h>
#include <exec/elf_load.h>
#include <ACPI/acpi.h>
#include <tss/tss.h>
#include <panic.h>

#include "kmain.h"

extern volatile struct limine_module_request module_request;
extern volatile struct limine_rsdp_request rsdp_request;

extern void init_sse(void);

static tty_t tty0;

void initrd_load(){ 
    if (!module_request.response || module_request.response->module_count == 0){
        serial_printf("No Modules Found by booloader\n");
        return;
    }

    struct limine_file* initrd = module_request.response->modules[0];
    tar_extract(initrd->address, initrd->size);
    return;
}

// we give the kernel a task here
void scheduler_start(void) {
    pic_unmask(0);
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));

    sched_bootstrap((void *)rsp);
}

void kmain() {
    asm volatile ("cli");
    init_sse();
    serial_init();
    pmm_init();

    reinit_paging();
    acpi_init();
    vfs_mount_root();
    initrd_load();
    psf_init("initrd/fonts/ttyfont.psf");
    stack_trace_load_symbols("initrd/etc/kernel.sym");
    
    tty0 = kernel_console_init();

    gdt_init();
    idt_init();
    tss_init();
    pit_init(1000);
    pic_init(32, 40);
    pic_unmask(2);

    scheduler_start();
    asm volatile ("sti");

    INode_t* tty_inode = device_get_by_name("tty0");

    if (tty_inode) {
        file_t* f0 = kmalloc(sizeof(file_t));
        f0->inode = tty_inode;
        f0->flags = O_RDWR;
        f0->offset = 0;
        f0->shared = 1;
        current_fd_table->fds[0] = f0;
        current_fd_table->fds[1] = f0;
        current_fd_table->fds[2] = f0;
    }

    sched_create_task(read_cr3(), (uint64_t)scheduler_reap, KERNEL_CS, KERNEL_SS);
    kernel_self_test();
    
    kprintf("\x1b[J");

    PS2_Keyboard_init();

    elf_sched(elf_get_from_path("initrd/bin/verdict"));
    for(;;){
        sched_yield();
    }

    return;
}
