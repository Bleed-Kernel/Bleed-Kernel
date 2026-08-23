#include <vendor/limine_bootloader/limine.h>
#include <boot/bootloader_interface/generic_bootloader.h>
#include <string.h>

/*
    This is crap, I thaught when i wrote this it would be a better idea than what it is,
    in thoery this is great but it doesnt really match the workflow i want

    there should be a boot.s file which the kernel starts at, detects the bootloader and
    jumps to the correct point or in the case of limine the bootloader entry point can be defined

    for grub we would set up 64 bit long mode and hhdm in this start function then move onto a normal boot
    for limine we dont need to do that so we would set the entry point to kmain()

    then kmain can populate the gbi structs
    jobs a gooden
    this file doenst achieve that but this system is a work in progress.
    take notes if you wannt try this korben, dont make my mistakes LMAO
*/

gbi_t g_gbi;

__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_executable_cmdline_request executable_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
};

static struct limine_internal_module initrd_module = {
    .string = "initrd.tar",
    .path   = "initrd.tar",
    .flags  = LIMINE_INTERNAL_MODULE_REQUIRED
};

static struct limine_internal_module *internal_module_ptrs[] = {
    &initrd_module
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 1,
    .internal_module_count = 1,
    .internal_modules = internal_module_ptrs,
};

static gbi_memmap_type_t translate_memmap_type(uint64_t limine_type)
{
    switch (limine_type) {
        case LIMINE_MEMMAP_USABLE:                 return GBI_MEMMAP_USABLE;
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:        return GBI_MEMMAP_ACPI_RECLAIMABLE;
        case LIMINE_MEMMAP_ACPI_NVS:                return GBI_MEMMAP_ACPI_NVS;
        case LIMINE_MEMMAP_BAD_MEMORY:              return GBI_MEMMAP_BAD_MEMORY;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:  return GBI_MEMMAP_BOOTLOADER_RECLAIMABLE;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:  return GBI_MEMMAP_KERNEL_AND_MODULES;
        case LIMINE_MEMMAP_FRAMEBUFFER:             return GBI_MEMMAP_FRAMEBUFFER;
        case LIMINE_MEMMAP_RESERVED:
        default:                                    return GBI_MEMMAP_RESERVED;
    }
}

static void translate_memmap(void)
{
    if (!memmap_request.response)
        return;

    struct limine_memmap_response *r = memmap_request.response;
    size_t count = r->entry_count;
    if (count > GBI_MAX_MEMMAP_ENTRIES)
        count = GBI_MAX_MEMMAP_ENTRIES;

    for (size_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = r->entries[i];
        g_gbi.memmap[i].base   = e->base;
        g_gbi.memmap[i].length = e->length;
        g_gbi.memmap[i].type   = translate_memmap_type(e->type);
    }
    g_gbi.memmap_entry_count = count;
}

static void translate_modules(void)
{
    if (!module_request.response)
        return;

    struct limine_module_response *r = module_request.response;
    size_t count = r->module_count;
    if (count > GBI_MAX_MODULES)
        count = GBI_MAX_MODULES;

    for (size_t i = 0; i < count; i++) {
        struct limine_file *f = r->modules[i];
        g_gbi.modules[i].address = (uint64_t)f->address;
        g_gbi.modules[i].size    = f->size;
        g_gbi.modules[i].path    = f->path;
        g_gbi.modules[i].cmdline = f->string;
    }
    g_gbi.module_count = count;
}

static void translate_framebuffer(void)
{
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0) {
        g_gbi.framebuffer.present = 0;
        return;
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    g_gbi.framebuffer.address = (uint64_t)fb->address;
    g_gbi.framebuffer.width   = fb->width;
    g_gbi.framebuffer.height  = fb->height;
    g_gbi.framebuffer.pitch   = fb->pitch;
    g_gbi.framebuffer.bpp     = fb->bpp;
    g_gbi.framebuffer.present = 1;
}

void gbi_init(void)
{
    memset(&g_gbi, 0, sizeof(g_gbi));
    g_gbi.bootloader = BOOTLOADER_LIMINE;

    if (bootloader_info_request.response) {
        g_gbi.bootloader_name    = bootloader_info_request.response->name;
        g_gbi.bootloader_version = bootloader_info_request.response->version;
    }

    if (hhdm_request.response)
        g_gbi.hhdm_offset = hhdm_request.response->offset;

    if (rsdp_request.response)
        g_gbi.rsdp_address = (uint64_t)rsdp_request.response->address;

    if (executable_address_request.response) {
        g_gbi.kernel_phys_base = executable_address_request.response->physical_base;
        g_gbi.kernel_virt_base = executable_address_request.response->virtual_base;
    }

    if (executable_cmdline_request.response)
        g_gbi.cmdline = executable_cmdline_request.response->cmdline;

    translate_framebuffer();
    translate_memmap();
    translate_modules();
}

const char *gbi_bootloader_name(bootloader_type_t type)
{
    switch (type) {
        case BOOTLOADER_LIMINE: return "Limine";
        case BOOTLOADER_GRUB2:  return "GRUB2";
        default:                return "Unknown";
    }
}