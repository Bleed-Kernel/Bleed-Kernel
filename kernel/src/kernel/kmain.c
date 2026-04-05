#include <stdint.h>
#include <stdio.h>
#include <ansii.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <drivers/ps2/PS2_mouse.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <string.h>
#include <mm/pmm.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <mm/smap.h>
#include <drivers/serial/serial.h>
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
#include <devices/type/mouse_device.h>
#include <ACPI/acpi.h>
#include <tss/tss.h>
#include <panic.h>
#include <ACPI/acpi_hpet.h>
#include <devices/type/kbd_device.h>
#include <kernel/kmain.h>
#include <kernel/bootargs.h>
#include <cpu/features/features.h>
#include <devices/type/serial_device.h>
#include <devices/type/hpet_device.h>
#include <drivers/ide/ide.h>
#include <fs/fat32/fat32.h>
#include <devices/type/blk_device.h>
#include <fs/vfs_mount.h>

#define KERNEL_BOOT_TTY_COUNT 4
#define KERNEL_MAX_LAZY_TTYS 12

extern volatile struct limine_module_request module_request;
extern volatile struct limine_rsdp_request rsdp_request;
extern volatile struct limine_executable_cmdline_request cmdline_request;

extern void avx_enable(void);
extern void sse_enable(void);
tty_t tty0;

static INode_t *g_shell_elf = NULL;
static char g_shell_path[256];
static uint8_t g_shell_spawned[KERNEL_MAX_LAZY_TTYS];
static volatile uint32_t g_lazy_spawn_pending_mask = 0;
static spinlock_t g_shell_spawn_lock = {0};

static int kernel_bind_stdio_to_tty(uint32_t tty_index) {
    char tty_name[16];
    snprintf(tty_name, sizeof(tty_name), "tty%u", tty_index);
    INode_t *tty_inode = device_get_by_name(tty_name);
    if (!tty_inode)
        return -1;

    fd_table_t *boot_fds = vfs_get_kernel_table();
    if (!boot_fds)
        return -1;

    file_t *f = kmalloc(sizeof(file_t));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    f->type = FD_TYPE_DEV;
    f->inode = tty_inode;
    f->flags = O_RDWR;
    f->offset = 0;
    f->shared = 2;

    boot_fds->fds[1] = f;
    boot_fds->fds[2] = f;
    return 0;
}

int kernel_spawn_shell_on_tty(uint32_t tty_index) {
    if (tty_index >= KERNEL_MAX_LAZY_TTYS)
        return -1;
    if (!g_shell_elf)
        return -1;
    if (g_shell_spawned[tty_index])
        return 0;

    if (kernel_bind_stdio_to_tty(tty_index) < 0)
        return -1;

    if (!elf_sched(g_shell_elf, 0, NULL))
        return -1;

    g_shell_spawned[tty_index] = 1;
    return 0;
}

void kernel_request_shell_spawn(uint32_t tty_index) {
    if (tty_index >= KERNEL_MAX_LAZY_TTYS)
        return;
    if (!g_shell_elf)
        return;

    unsigned long irq = irq_push();
    spinlock_acquire(&g_shell_spawn_lock);
    if (!g_shell_spawned[tty_index]) {
        g_lazy_spawn_pending_mask |= (1u << tty_index);
    }
    spinlock_release(&g_shell_spawn_lock);
    irq_restore(irq);
}

int kernel_has_shell_spawn_request(void) {
    return g_lazy_spawn_pending_mask != 0;
}

void kernel_service_shell_spawn_requests(void) {
    uint32_t pending = 0;

    unsigned long irq = irq_push();
    spinlock_acquire(&g_shell_spawn_lock);
    pending = g_lazy_spawn_pending_mask;
    g_lazy_spawn_pending_mask = 0;
    spinlock_release(&g_shell_spawn_lock);
    irq_restore(irq);

    if (!pending)
        return;

    for (uint32_t i = 0; i < KERNEL_MAX_LAZY_TTYS; i++) {
        if (pending & (1u << i)) {
            (void)kernel_spawn_shell_on_tty(i);
        }
    }
}

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
        memset(g_shell_spawned, 0, sizeof(g_shell_spawned));
        memset(g_shell_path, 0, sizeof(g_shell_path));
        strncpy(g_shell_path, target_path, sizeof(g_shell_path) - 1);
        g_shell_path[sizeof(g_shell_path) - 1] = '\0';

        kprintf(LOG_INFO "Starting init process %s\n", g_shell_path);

        g_shell_elf = elf_get_from_path(g_shell_path);
        if (!g_shell_elf) {
            kprintf(LOG_ERROR "Failed to load ELF: %s\n", g_shell_path);
            return;
        }

        for (uint32_t i = 1; i < KERNEL_BOOT_TTY_COUNT; i++) {
            if (tty_create_framebuffer(NULL) < 0) {
                kprintf(LOG_WARN "Failed to create tty%u\n", i);
                break;
            }
        }

        if (kernel_spawn_shell_on_tty(0) < 0) {
            kprintf(LOG_WARN "Failed to start shell on tty0\n");
        }

        (void)tty_set_active_index(0);
        (void)kernel_bind_stdio_to_tty(0);
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
    if (stack_trace_load_symbols("initrd/etc/kernel.sym") < 0) {
        kprintf(LOG_WARN "Failed to load kernel symbols from initrd/etc/kernel.sym\n");
    } else {
        kprintf(LOG_OK "Kernel symbols loaded from initrd/etc/kernel.sym\n");
    }
    reinit_paging();
    acpi_init();
    
    tty0 = kernel_console_init();

    gdt_init();
    idt_init();
    tss_init();
    syscall_init();
    fb_device_init();

    bootargs_init(cmdline_request.response->cmdline);
    acpi_init_hpet();
    hpet_device_init();

    kbd_device_init();
    mouse_device_init();
    PS2_Keyboard_init();
    PS2_Mouse_init();
    serial_device_register();

/*
    HARDCODED SHITE :/
    - make mount and unmount a systemcall
    - mount and unmount programs so we can do it manually
    - some sort of file system table?
*/
    ide_init();

    INode_t *partition_node = device_get_by_name("hda1");

    if (partition_node) {
        blk_device_t *blk = (blk_device_t *)partition_node->internal_data;
        vfs_mkdir("/mnt"); 

        
        if (vfs_mount("/mnt", blk) == 0) {
            serial_printf(LOG_OK "fat32: Mounted to /mnt\n");
        }
    }

    scheduler_start();

    INode_t* tty_inode = device_get_by_name("tty0");
    tty_device_init(tty_inode);

    asm volatile ("sti");

    sched_create_task(read_cr3(), (uint64_t)scheduler_reap, KERNEL_CS, KERNEL_SS, "reaper");

    supervisor_memory_protection_init();
    UMIP_init();
    shell_start();

    for (;;) {
        if (kernel_has_shell_spawn_request()) {
            kernel_service_shell_spawn_requests();
        }
        sched_yield(get_current_task());
    }
}
