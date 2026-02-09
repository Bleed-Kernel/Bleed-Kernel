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
#include <mm/smap.h>
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
#include <ACPI/acpi_time.h>
#include <mm/vmm.h>
#include <exec/elf_load.h>
#include <devices/type/fb_device.h>
#include <devices/type/kbd_device.h>
#include <ACPI/acpi.h>
#include <tss/tss.h>
#include <panic.h>
#include <ACPI/acpi_hpet.h>
#include <devices/type/kbd_device.h>
#include <kernel/kmain.h>
#include <kernel/bootargs.h>

extern volatile struct limine_module_request module_request;
extern volatile struct limine_rsdp_request rsdp_request;
extern volatile struct limine_executable_cmdline_request cmdline_request;

extern void avx_enable(void);
extern void sse_enable(void);
tty_t tty0;

void initrd_load(){ 
    if (!module_request.response || module_request.response->module_count == 0){
        kprintf("No Modules Found by booloader\n");
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

void shell_start() {
    const char* target_path = NULL;
    char path_buffer[256];

    if (bootargs_is("shell-path", "default")) {
        kprintf(LOG_INFO "Loading default shell path from initrd/etc/shell...\n");
        
        int sfd = vfs_open("initrd/etc/shell", O_RDONLY);
        if (sfd < 0) {
            kprintf(LOG_ERROR "Could not open initrd/etc/shell. System halted.\n");
            return; 
        }

        memset(path_buffer, 0, sizeof(path_buffer));
        long bytes_read = vfs_read(sfd, path_buffer, sizeof(path_buffer) - 1);
        vfs_close(sfd);

        if (bytes_read <= 0) {
            kprintf(LOG_ERROR "Shell path file is empty or unreadable.\n");
            return;
        }

        for (int i = 0; i < bytes_read; i++) {
            if (path_buffer[i] == '\n' || path_buffer[i] == '\r' || path_buffer[i] == ' ') {
                path_buffer[i] = '\0';
                break;
            }
        }
        
        target_path = path_buffer;
    } else {
        target_path = bootargs_get("shell-path");
    }

    if (target_path && target_path[0] != '\0') {
        kprintf(LOG_INFO "Starting init process: %s\n", target_path);

        void* elf = elf_get_from_path(target_path);
        if (elf) {
            elf_sched(elf);
        } else {
            kprintf(LOG_ERROR "Failed to load ELF: %s\n", target_path);
        }
    } else {
        kprintf(LOG_ERROR "No valid shell path provided.\n");
    }
}

void kmain() {
    asm volatile ("cli");
    sse_enable();
    avx_enable();
    serial_init();
    pmm_init();
    vfs_mount_root();
    initrd_load();
    psf_init("initrd/fonts/ttyfont.psf");
    stack_trace_load_symbols("initrd/etc/kernel.sym");
    reinit_paging();
    acpi_init();
    
    tty0 = kernel_console_init();

    gdt_init();
    idt_init();
    tss_init();

    bootargs_init(cmdline_request.response->cmdline);
    if (bootargs_is("timer", "hpet"))
        acpi_init_hpet();
    else
        pit_init(1000);

    pic_init(32, 40);
    pic_unmask(2);
    pic_unmask(1);

    kbd_device_init();
    PS2_Keyboard_init();

    scheduler_start();
    fb_device_init();

    INode_t* tty_inode = device_get_by_name("tty0");
    tty_device_init(tty_inode);

    asm volatile ("sti");

    sched_create_task(read_cr3(), (uint64_t)scheduler_reap, KERNEL_CS, KERNEL_SS, "reaper");
    
    supervisor_memory_protection_init();

    if (bootargs_is("self-test", "yes")) kernel_self_test(); 
    shell_start();

    for (;;) {
        sched_yield();
    }
}
