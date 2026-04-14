#include <drivers/ps2/PS2_mouse.h>
#include <cpu/io.h>
#include <input/mouse_dispatch.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define PS2_STATUS_OUT_FULL  0x01
#define PS2_STATUS_IN_FULL   0x02
#define PS2_STATUS_AUX_DATA  0x20

#define PS2_CMD_ENABLE_AUX   0xA8
#define PS2_CMD_DISABLE_AUX  0xA7
#define PS2_CMD_READ_CFG     0x20
#define PS2_CMD_WRITE_CFG    0x60
#define PS2_CMD_WRITE_MOUSE  0xD4

// PS/2 controller config bits
#define PS2_CFG_KBD_IRQ  (1 << 0)
#define PS2_CFG_AUX_IRQ  (1 << 1)
#define PS2_CFG_AUX_CLOCK (1 << 5)

// Mouse device commands
#define MOUSE_CMD_RESET        0xFF
#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE       0xF4
#define MOUSE_CMD_DISABLE      0xF5
#define MOUSE_ACK              0xFA
#define MOUSE_SELF_TEST_OK     0xAA

#define PS2_MOUSE_SENS_NUM 1
#define PS2_MOUSE_SENS_DEN 2

static uint8_t  packet[3];
static uint8_t  packet_index = 0;
static int32_t  rem_dx = 0;
static int32_t  rem_dy = 0;
static int      mouse_present = 0;

// helpers
static void ps2_flush(void) {
    int limit = 128;
    while (limit-- > 0 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL))
        (void)inb(PS2_DATA_PORT);
}

static void ps2_wait_write(void) {
    int t = 100000;
    while (t-- > 0 && (inb(PS2_STATUS_PORT) & PS2_STATUS_IN_FULL));
}

static int ps2_wait_read(void) {
    int t = 100000;
    while (t-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL)
            return 0;
    }
    return -1;
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

static int ps2_read_data(uint8_t *out) {
    if (ps2_wait_read() < 0) return -1;
    *out = inb(PS2_DATA_PORT);
    return 0;
}

// Send a byte to the mouse device, wait for ACK, will retry 3 times
static int mouse_send(uint8_t cmd) {
    uint8_t ack;
    int retries = 3;
    while (retries-- > 0) {
        ps2_write_cmd(PS2_CMD_WRITE_MOUSE);
        ps2_write_data(cmd);
        if (ps2_wait_read() < 0) continue;
        ack = inb(PS2_DATA_PORT);
        if (ack == MOUSE_ACK) return 0;
    }
    return -1;
}

// sensativity scaling
static inline int16_t ps2_scale_delta(int16_t delta, int32_t *rem) {
    int32_t accum = ((int32_t)delta * PS2_MOUSE_SENS_NUM) + *rem;
    int32_t scaled = accum / PS2_MOUSE_SENS_DEN;
    *rem = accum % PS2_MOUSE_SENS_DEN;
    return (int16_t)scaled;
}

void PS2_Mouse_init(void) {
    uint8_t cfg = 0;
    uint8_t result = 0;

    mouse_present = 0;

    ps2_write_cmd(PS2_CMD_ENABLE_AUX);
    ps2_flush();

    ps2_write_cmd(PS2_CMD_READ_CFG);
    if (ps2_read_data(&cfg) < 0) return;

    cfg |=  PS2_CFG_AUX_IRQ;    // enable IRQ12 
    cfg &= ~PS2_CFG_AUX_CLOCK;  // enable aux clock (0 = on) 

    ps2_write_cmd(PS2_CMD_WRITE_CFG);
    ps2_write_data(cfg);

    if (mouse_send(MOUSE_CMD_RESET) < 0) {
        // No mouse or no aux port disable aux IRQ and forget
        ps2_write_cmd(PS2_CMD_READ_CFG);
        if (ps2_read_data(&cfg) == 0) {
            cfg &= ~PS2_CFG_AUX_IRQ;
            cfg |=  PS2_CFG_AUX_CLOCK; // disable aux clock again 
            ps2_write_cmd(PS2_CMD_WRITE_CFG);
            ps2_write_data(cfg);
        }
        ps2_write_cmd(PS2_CMD_DISABLE_AUX);
        return;
    }

    // Consume the self-test result byte (0xAA) and device ID (0x00) 
    ps2_read_data(&result); // 0xAA 
    ps2_read_data(&result); // 0x00 = standard PS/2 mouse 

    if (mouse_send(MOUSE_CMD_SET_DEFAULTS) < 0) return;
    if (mouse_send(MOUSE_CMD_ENABLE)        < 0) return;

    mouse_present  = 1;
    packet_index   = 0;
    rem_dx         = 0;
    rem_dy         = 0;
}


void PS2_Mouse_Interrupt(uint8_t irq) {
    (void)irq;
    if (!mouse_present) return;

    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUT_FULL)) return;
    if (!(status & PS2_STATUS_AUX_DATA)) return; // belongs to keyboard 

    uint8_t data = inb(PS2_DATA_PORT);

    // Byte 0: bit 3 must always be 1
    if (packet_index == 0) {
        if (!(data & (1 << 3))) return;
        packet[0] = data;
        packet_index = 1;
        return;
    }

    packet[packet_index++] = data;
    if (packet_index < 3) return;
    packet_index = 0;

    // Decode deltas with sign extension 
    int16_t dx = (packet[0] & (1 << 4)) ? (int16_t)packet[1] - 256
                                         : (int16_t)packet[1];
    int16_t dy = (packet[0] & (1 << 5)) ? (int16_t)packet[2] - 256
                                         : (int16_t)packet[2];

    // Clamp on overflow bits 
    if (packet[0] & (1 << 6)) dx = (packet[0] & (1 << 4)) ? -255 : 255;
    if (packet[0] & (1 << 7)) dy = (packet[0] & (1 << 5)) ? -255 : 255;

    dx = ps2_scale_delta(dx, &rem_dx);
    dy = ps2_scale_delta(dy, &rem_dy);

    mouse_event_t ev = {
        .dx      = dx,
        .dy      = -dy,
        .wheel   = 0,
        .buttons =
            (packet[0] & (1 << 0) ? MOUSE_BTN_LEFT   : 0) |
            (packet[0] & (1 << 1) ? MOUSE_BTN_RIGHT  : 0) |
            (packet[0] & (1 << 2) ? MOUSE_BTN_MIDDLE : 0)
    };

    mouse_input_dispatch(&ev);
}