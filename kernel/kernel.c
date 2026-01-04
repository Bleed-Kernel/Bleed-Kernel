#include <stdint.h>
#include <stdio.h>
#include <ansii.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <string.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <mm/paging.h>
#include <self-test/ktests.h>
#include <drivers/serial/serial.h>
#include <drivers/pic/pic.h>
#include <drivers/pit/pit.h>
#include <fs/vfs.h>
#include <status.h>
#include <fs/archive/tar.h>
#include <sys/sleep.h>
#include <fonts/psf.h>
#include <drivers/framebuffer/framebuffer.h>
#include <devices/type/tty_device.h>
#include <devices/device_io.h>
#include <console/console.h>
#include <sched/scheduler.h>
#include <threads/exit.h>
#include <cpu/stack_trace.h>
#include <syscalls/syscall.h>
#include <exec/elf_load.h>
#include <tss/tss.h>
#include <panic.h>

extern volatile struct limine_module_request module_request;
extern void init_sse(void);

static tty_t tty0;
static tty_fb_backend_t tty0_backend;

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

task_t *load_elf_from_initrd(const char *path){
    INode_t *file = NULL;
    path_t filepath = vfs_path_from_abs(path);

    vfs_lookup(&filepath, &file);
    paddr_t cr3 = paging_create_address_space();

    uintptr_t entry;
    if (file != NULL) elf_load(file, cr3, &entry);
    return sched_create_task(cr3, entry, USER_CS, USER_SS);
}

void kernel_self_test(){
    paging_test_self_test();
    pmm_test_self_test();
    pit_test_self_test();
    scheduler_test_self_test();
    vfs_test_self_test();

    tty0.ops->clear(&tty0);
}

void console_init(){
    fb_console_t fb = {
        .pixels = framebuffer_get_addr(0),
        .width = framebuffer_get_width(0),
        .height = framebuffer_get_height(0),
        .pitch = framebuffer_get_pitch(0),
        .font = psf_get_current_font(),
        .fg = 0xFFFFFF,
        .bg = 0x000000,
    };

    tty_init_framebuffer(&tty0, &tty0_backend, "tty0", &fb, TTY_ECHO | TTY_CANNONICAL);
    device_register(&tty0.device);

    device_t *tty = device_get_by_name("tty0");
    console_set(tty);
}

void kmain() {
    asm volatile ("cli");
    init_sse();
    serial_init();
    pmm_init();

    reinit_paging();
    vfs_mount_root();
    initrd_load();
    psf_init("initrd/fonts/ttyfont.psf");
    stack_trace_load_symbols("initrd/etc/kernel.sym");
    console_init();

    gdt_init();
    idt_init();
    tss_init();
    pit_init(1000);
    pic_init(32, 40);
    pic_unmask(2);

    scheduler_start();
    asm volatile ("sti");

    sched_create_task(read_cr3(), (uint64_t)scheduler_reap, KERNEL_CS, KERNEL_SS);
    kernel_self_test();
    PS2_Keyboard_init();

    load_elf_from_initrd("initrd/bin/verdict");

    for(;;){
        sched_yield();
    }

    ke_panic("Kernel Main Thread Died");
    return;
}
