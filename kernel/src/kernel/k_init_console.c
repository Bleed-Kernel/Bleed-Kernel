#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <console/console.h>

static tty_t tty0;
static tty_fb_backend_t tty0_backend;

tty_t kernel_console_init(){
    fb_console_t fb = {
        .pixels = framebuffer_get_addr(0),
        .width = framebuffer_get_width(0),
        .height = framebuffer_get_height(0),
        .pitch = framebuffer_get_pitch(0),
        .font = psf_get_current_font(),
        .fg = 0xFFFFFF,
        .bg = 0x000000,
    };

    tty_init_framebuffer(&tty0, &tty0_backend, &fb, TTY_CANNONICAL);
    device_register(&tty0.device, "tty0");

    INode_t *tty = device_get_by_name("tty0");
    console_set(tty, tty0);
    
    return tty0;
}