#include <vendor/limine_bootloader/limine.h>
#include <drivers/framebuffer/framebuffer.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ansii.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

void *framebuffer_get_addr(int idx) {
    return framebuffer_request.response->framebuffers[idx]->address;
}

uint64_t framebuffer_get_pitch(int idx) {
    return framebuffer_request.response->framebuffers[idx]->pitch;
}

uint64_t framebuffer_get_width(int idx) {
    return framebuffer_request.response->framebuffers[idx]->width;
}

uint64_t framebuffer_get_height(int idx) {
    return framebuffer_request.response->framebuffers[idx]->height;
}

uint64_t framebuffer_get_bpp(int idx) {
    return framebuffer_request.response->framebuffers[idx]->bpp;
}