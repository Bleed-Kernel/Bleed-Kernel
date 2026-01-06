// kernel/boot/limine_requests.c
#include <vendor/limine_bootloader/limine.h>

__attribute__((used, section(".limine_requests")))
struct limine_internal_module initrd_module = {
    .string = "initrd.tar",
    .path   = "initrd.tar",
    .flags  = LIMINE_INTERNAL_MODULE_REQUIRED
};

__attribute__((used, section(".limine_requests")))
struct limine_internal_module *internal_module_ptrs[] = {
    &initrd_module
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 1,
    .internal_module_count = 1,
    .internal_modules = internal_module_ptrs,
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests")))
volatile uint64_t limine_base_revision[] =
    LIMINE_BASE_REVISION(4);
