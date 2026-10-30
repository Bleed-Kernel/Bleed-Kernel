#include <stdbool.h>
#include <stdint.h>

#include <boot/bootloader_interface/generic_bootloader.h>
#include <boot/bootlogger/bootlogger.h>
#include <drivers/serial/serial.h>
#include <kernel/bootargs.h>
#include <drivers/framebuffer/framebuffer.h>
#include <ansii.h>

#include <cpu/features/simd.h>
#include <cpu/features/features.h>
#include <cpu/features/fpu.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <tss/tss.h>
#include <cpu/percpu.h>
#include <cpu/smp.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/smap.h>

#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <fs/data_image.h>
#include <fs/archive/tar.h>
#include <fonts/psf.h>
#include <boot/splash_image.h>
#include <kernel/exception/panic_screen.h>
#include <kernel/exception/stack_trace.h>

#include <ACPI/acpi.h>
#include <ACPI/acpi_hpet.h>
#include <ACPI/acpi_bgrt.h>

#include <drivers/ps2/PS2_mouse.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb_storage.h>
#include <drivers/i2c/dw_i2c.h>
#include <drivers/i2c/i2c_trackpad.h>
#include <devices/type/null_device.h>
#include <devices/type/hpet_device.h>
#include <devices/type/mouse_device.h>
#include <devices/type/kbd_device.h>
#include <devices/type/fb_device.h>
#include <devices/type/serial_tty_device.h>
#include <devices/type/power_device.h>
#include <drivers/cros_ec/cros_ec.h>
#include <devices/type/serial_device.h>
#include <devices/type/tty_device.h>
#include <devices/type/net_device.h>

#include <drivers/ide/ide.h>
#include <drivers/ahci/ahci.h>
#include <drivers/nvme/nvme.h>
#include <drivers/ac97/ac97.h>
#include <drivers/e1000/e1000.h>
#include <net/net.h>
#include <devices/type/blk_device.h>

#include <sched/scheduler.h>

#include <exec/elf_load.h>
#include <syscalls/syscall.h>
#include <kernel/kmain.h>
#include <kernel/exception/panic.h>

// to be clear this is used when its too early for the kernel to properly panic
// realistically, it should never run.
#define KERNEL_STOP()                           \
    do {                                        \
        __asm__ volatile (                      \
            "cli\n\t"                           \
            "1: hlt\n\t"                        \
            "jmp 1b"                            \
        );                                      \
    } while (0)

static simd_level_t sse_level;

void init_processor_state(){
    BLOG_INFO("Attempting to initialise GDT");
    void* gdt_ptr = gdt_init(this_cpu());
    if (gdt_ptr){
        BLOG_OKF("GDT Ready -- %p", gdt_ptr);
        serial_printf(LOG_OK "Global Descriptor Table Loaded (GDTR=%p)\n", gdt_ptr);
    }else{
        BLOG_FAIL("GDT returned a NULL Pointer, this is unrecoverable. system halted");
        KERNEL_STOP();
    }

    BLOG_INFO("Attempting to initialise IDT");
    uint64_t idt_ptr = idt_init();
    if (idt_ptr){
        BLOG_OKF("IDT Ready -- %p", idt_ptr);
        serial_printf(LOG_OK "Interrupt Descriptor Table Loaded (IDTR=%p)\n", idt_ptr);
    }else{
        BLOG_FAIL("IDT returned a NULL Pointer, this is unrecoverable. system halted");
        KERNEL_STOP();
    }

    // on most Hypervisors and probably all real hardware it will just triple fault but its really an edge
    // case, who knows what a rouge hypervisor will do in the future. Virtualbox.

    BLOG_INFO("Attempting to initialise TSS");
    tss_init(this_cpu());
    BLOG_OK("TSS Ready");
    serial_printf(LOG_OK "TSS Ready\n");

    BLOG_INFO("Attempting to initialise the Physical Memory Manager");
    pmm_init();
    BLOG_OK("Physical Memory Manager Ready");
    serial_printf(LOG_OK "Physical Memory Manager Ready\n");

    BLOG_INFO("Attempting to initialise Paging");
    init_paging();
    BLOG_OK("Paging Ready");
    serial_printf(LOG_OK "Paging Ready\n");

    BLOG_OKF("SIMD is Ready\n\tYour Processor Supports %s", simd_level_name(sse_level));
    serial_printf(LOG_OK "%s Ready\n", simd_level_name(sse_level));

    fpu_init();
}

void init_ramdisk(){
    BLOG_INFO("Mounting Root");
    vfs_mount_root();
    BLOG_OK("Root Mounted");

    BLOG_INFO("Locating initrd from Bootloader");
    if (g_gbi.module_count == 0){
        ke_panic(NULL, "Bootloader Modules Empty, Initrd is missing");
        return;
    }
    BLOG_OK("Located initrd");

    BLOG_INFO("Attempting to Extract initrd");
    gbi_module_t *initrd = &g_gbi.modules[0];
    tar_extract((void *)initrd->address, initrd->size);
    BLOG_OK("initrd extracted successfully");

    BLOG_INFO("Loading Kernel Resources");

    bool splash_display = false;

    if (bootargs_is("splash", "no")){
        splash_display = true;
    } else if (bootargs_is("splash", "bleed")) {
        splash_display = display_splash_screen("initrd/boot/splash.bgra", 256, 256);

        if (splash_display){
            bconsole_reserve_rows(252);
            BLOG_OK("\t - BGRA Splash Image");
        }else{
            BLOG_FAIL("\t x BGRA Splash Image");
        }
    } else {
        if (acpi_bgrt_present() && acpi_bgrt_display_logo()) {
            splash_display = true;
            BLOG_OK("\t - Firmware (ACPI BGRT) Splash Image");
        } else {
            splash_display = display_splash_screen("initrd/boot/splash.bgra", 256, 256);

            if (splash_display){
                bconsole_reserve_rows(252);
                BLOG_OK("\t - BGRA Splash Image");
            }else{
                BLOG_FAIL("\t x BGRA Splash Image");
            }
        }
    }

    bool font_init = false;
    if (bootargs_get("ttyfont") != NULL){
        font_init = psf_init(bootargs_get("ttyfont"));
    }else{
        font_init = psf_init("initrd/fonts/ttyfont.psf");
    }

    if (font_init)
        BLOG_OK("\t - PSF Font");
    else
        BLOG_FAIL("\t x PSF Font");

    bool kernel_symtab = stack_trace_load_symbols("initrd/etc/kernel.sym");
    if (kernel_symtab)
        BLOG_OK("\t - Kernel Symbol Table");
    else
        BLOG_FAIL("\t x Kernel Symbol Table");

    if (splash_display && font_init && kernel_symtab){
        BLOG_OK("Kernel Resources Ready");
    }
}

void init_firmware_relationship(){
    BLOG_INFO("Attempting to initialise ACPI");
    acpi_init();
    BLOG_OK("ACPI Ready");
}

void init_devices(){
    BLOG_INFO("Starting framebuffer Device");
    fb_device_init();
    BLOG_OK("framebuffer Device Ready");

    null_device_init();

    BLOG_INFO("Starting HPET Device");
    acpi_init_hpet();
    hpet_device_init();
    BLOG_OK("HPET Device Ready");

    BLOG_INFO("Starting keyboard Device");
    PS2_Keyboard_init();
    kbd_device_init();
    BLOG_OK("keyboard Device Ready");

    BLOG_INFO("Starting mouse Device");
    PS2_Mouse_init();
    mouse_device_init();
    BLOG_OK("mouse Device Ready");

    BLOG_INFO("Enumerating USB Controllers");
    usb_init();

    BLOG_INFO("Enumerating I2C Controllers");
    dw_i2c_init();
    i2c_trackpad_init();
    if (i2c_trackpad_present())
        BLOG_OK("I2C trackpad Ready");

    BLOG_INFO("Starting serial Device");
    serial_device_register();
    BLOG_OK("serial Device Ready");
    if (serial_log_separate())
        BLOG_OK("serial log on COM3, shell on COM1");

    if (serial_tty_init() == 0)
        BLOG_OK("serial terminal Ready");

    BLOG_INFO("Enumerating AC97 Controller");
    ac97_init();

    BLOG_INFO("Enumerating Network Controllers");
    e1000_init();
    net_device_init();

    BLOG_INFO("Starting power Device");
    cros_ec_init();
    power_device_init();
    BLOG_OK("power Device Ready");

    tty_device_init();
}

void init_block_devices(){
    vfs_mkdir("/mnt");

    BLOG_INFO("Block Devices");
    ide_init();
    INode_t *hda1 = device_get_by_name("hda1");
    if (hda1){
        vfs_mount("/mnt/ide", hda1);
        BLOG_OK("\t - IDE Device Ready");
    }

    ahci_init();
    INode_t *sda1 = device_get_by_name("sda1");
    if (sda1){
        vfs_mount("/mnt/sata", sda1);
        BLOG_OK("\t - SATA Device Ready");
    }

    nvme_init();
    INode_t *nvme0p1 = device_get_by_name("nvme0p1");
    if (nvme0p1){
        vfs_mount("/mnt/nvme", nvme0p1);
        BLOG_OK("\t - NVMe Device Ready");
    }

    // usb_init already found the drives, usb_service does the same for ones plugged in later
    usb_storage_automount();
    data_image_scan();

    BLOG_OK("Block Device Probing");
}

void init_multitasking(){
    BLOG_INFO("Attempting to start the scheduler");
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    sched_bootstrap((void *)rsp);
    BLOG_OK("Scheduler Started");

    BLOG_INFO("Attempting to start the Task Reaper");
    task_t *reaper = NULL;
    reaper = sched_create_task(read_cr3(), (uint64_t)scheduler_reap, KERNEL_CS, KERNEL_SS, "reaper_service");

    if (reaper != NULL)
        BLOG_OK("Reaper Started");
    else
        ke_panic(NULL, "Task Reaper did not start!");
}

bool init_userspace(){
    BLOG_INFO("Attempting to enable SYSCALL");
    syscall_init();
    BLOG_OK("SYSCALL enabled");

    if (!bootargs_has("no-smap")){
        BLOG_INFO("Attempting to enable SMAP/SMEP");
        int smap_status = SMAP_init();

        switch (smap_status){
            case SMAP_MISSING:
                BLOG_WARN("SMAP not Supported");
                break;
            case SMEP_MISSING:
                BLOG_WARN("SMEP not Supported");
                break;
            case BOTH_MISSING:
                BLOG_WARN("SMAP and SMEP are not Supported");
                break;
            case FULLY_SUPPORTED:
                BLOG_OK("SMAP Enabled");
                BLOG_OK("SMEP Enabled");
                break;
            default:
                BLOG_WARN("SMAP/SMEP status is Unknown?");
        }
    }

    if (!bootargs_has("no-umip")){
        BLOG_INFO("Attempting to enable UMIP");
        int umip_status = UMIP_init();
        if (umip_status == 0){
            BLOG_OK("UMIP Enabled");
        }else{
            BLOG_WARN("UMIP Not Supported");
        }
    }

    const char* shell_path = bootargs_get("shell");
    const char* init_path  = bootargs_get("init");
    if (!shell_path) shell_path = DEFAULT_SHELL;
    if (!init_path)  init_path  = DEFAULT_INIT;

    INode_t *shell_elf = elf_get_from_path(shell_path);
    INode_t *init_elf  = elf_get_from_path(init_path);
    bool     same      = shell_elf && init_elf == shell_elf;

    // the shell stays hidden behind the splash until init takes the screen
    if (!same) tty_set_graphics(1);

    if (panic_screen_load())
        BLOG_OK("Panic screen loaded");
    else
        BLOG_WARN("Panic screen failed to load, panics stay on the console");

    BLOG_INFO("Attempting to start shell");
    task_t *shell = elf_sched(shell_elf, 0, NULL);
    if (shell != NULL)
        BLOG_OK("Shell Task started!");
    else
        BLOG_WARNF("Failure to start shell '%s'", shell_path);

    if (serial_present() && !bootargs_has("no-serial-shell")) {
        BLOG_INFO("Attempting to start serial shell");
        if (serial_tty_spawn(shell_elf))
            BLOG_OK("Serial shell started on ttyS0");
        else
            BLOG_WARN("Failure to start serial shell");
    }

    // shell=x init=x only wants the one instance
    if (same && shell) {
        BLOG_INFO("Init is the shell, not starting it twice");
    } else {
        BLOG_INFO("Attempting to start init");
        task_t *init = elf_sched(init_elf, 0, NULL);
        fb_device_hold_until(init);
        if (init != NULL)
            BLOG_OK("Init Task started!");
        else if (!shell)
            ke_panic(NULL, "Failure to Start listed init task");
        else
            BLOG_WARNF("Failure to start init '%s'", init_path);
    }
    vfs_drop(init_elf);
    vfs_drop(shell_elf);

    // after init so it keeps the pid it always had
    if (sched_create_task(read_cr3(), (uint64_t)power_button_watch, KERNEL_CS, KERNEL_SS, "power_service"))
        BLOG_OK("Power watchdog started");

    if (sched_create_task(read_cr3(), (uint64_t)usb_hotplug_watch, KERNEL_CS, KERNEL_SS, "usb_service"))
        BLOG_OK("USB hotplug service started");

    if (netif_default() &&
        sched_create_task(read_cr3(), (uint64_t)net_service, KERNEL_CS, KERNEL_SS, "net_service"))
        BLOG_OK("Network service started");

    if (i2c_trackpad_present() &&
        sched_create_task(read_cr3(), (uint64_t)i2c_trackpad_watch, KERNEL_CS, KERNEL_SS, "i2c_service"))
        BLOG_OK("I2C trackpad task started");

    return true;
}

__attribute__((noreturn))
void kmain(void){
    asm volatile("cli");
    cpu_bsp_early_init();
    
    sse_level = simd_enable();
    gbi_init();
    
    serial_init();
    bootargs_init(g_gbi.cmdline);
    framebuffer_apply_bootargs();

    if (bootargs_has("verbose")){
        bconsole_init();
    }
    bootargs_validate();

    BLOG_INFO("Starting the Bleed Kernel\n\t\
by Isabella 'Mellurboo' Wilson\n\t\
admin@bleedkernel.com\n\t\
Licenced under GPLv3\n");

    init_processor_state();
    init_firmware_relationship();
    init_ramdisk();
    init_devices();
    init_block_devices();
    init_multitasking();
    asm volatile("sti");

    init_userspace();

    if (!bootargs_has("no-smp")){
        smp_init();
    }
    splash_finish();
    kernel_console_init();
    
    for(;;){
        sched_block(get_current_task());
    }
}