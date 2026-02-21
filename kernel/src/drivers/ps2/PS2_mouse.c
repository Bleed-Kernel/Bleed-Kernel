#include <drivers/ps2/PS2_mouse.h>
#include <cpu/io.h>
#include <input/mouse_dispatch.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02

#define PS2_CMD_ENABLE_AUX_PORT 0xA8
#define PS2_CMD_READ_CFG        0x20
#define PS2_CMD_WRITE_CFG       0x60
#define PS2_CMD_WRITE_MOUSE     0xD4

#define PS2_MOUSE_CMD_SET_DEFAULTS    0xF6
#define PS2_MOUSE_CMD_ENABLE_REPORT   0xF4
#define PS2_MOUSE_ACK                 0xFA

static uint8_t packet[3];
static uint8_t packet_index = 0;

static inline void ps2_wait_input_ready(void) {
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_IN_FULL) {
    }
}

static inline int ps2_wait_output_ready(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL)
            return 0;
    }
    return -1;
}

static inline void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_input_ready();
    outb(PS2_COMMAND_PORT, cmd);
}

static inline void ps2_write_data(uint8_t data) {
    ps2_wait_input_ready();
    outb(PS2_DATA_PORT, data);
}

static inline int ps2_read_data(uint8_t *out) {
    if (ps2_wait_output_ready() < 0)
        return -1;
    *out = inb(PS2_DATA_PORT);
    return 0;
}

static void ps2_mouse_write(uint8_t value) {
    ps2_write_cmd(PS2_CMD_WRITE_MOUSE);
    ps2_write_data(value);
}

static int ps2_mouse_expect_ack(void) {
    uint8_t response = 0;
    if (ps2_read_data(&response) < 0)
        return -1;
    return response == PS2_MOUSE_ACK ? 0 : -1;
}

void PS2_Mouse_init(void) {
    uint8_t cfg = 0;

    ps2_write_cmd(PS2_CMD_ENABLE_AUX_PORT);

    ps2_write_cmd(PS2_CMD_READ_CFG);
    if (ps2_read_data(&cfg) < 0)
        return;

    cfg |= (1 << 1);    // IRQ12
    cfg &= ~(1 << 5);   // mouse clock

    ps2_write_cmd(PS2_CMD_WRITE_CFG);
    ps2_write_data(cfg);

    ps2_mouse_write(PS2_MOUSE_CMD_SET_DEFAULTS);
    (void)ps2_mouse_expect_ack();

    ps2_mouse_write(PS2_MOUSE_CMD_ENABLE_REPORT);
    (void)ps2_mouse_expect_ack();

    packet_index = 0;
}

void PS2_Mouse_Interrupt(uint8_t irq) {
    (void)irq;

    uint8_t data = inb(PS2_DATA_PORT);

    // Byte 0 must have the always-1 sync bit set. If not, stay unsynced.
    if (packet_index == 0) {
        if (!(data & (1 << 3)))
            return;

        packet[0] = data;
        packet_index = 1;
        return;
    }

    packet[packet_index++] = data;
    if (packet_index < 3)
        return;

    packet_index = 0;

    int16_t dx;
    int16_t dy;

    if (packet[0] & (1 << 4))
        dx = (int16_t)packet[1] - 256;
    else
        dx = (int16_t)packet[1];

    if (packet[0] & (1 << 5))
        dy = (int16_t)packet[2] - 256;
    else
        dy = (int16_t)packet[2];

    if (packet[0] & (1 << 6))
        dx = (packet[0] & (1 << 4)) ? -255 : 255;
    if (packet[0] & (1 << 7))
        dy = (packet[0] & (1 << 5)) ? -255 : 255;

    mouse_event_t ev = {
        .dx = dx,
        .dy = -dy,
        .wheel = 0,
        .buttons =
            (packet[0] & (1 << 0) ? MOUSE_BTN_LEFT : 0) |
            (packet[0] & (1 << 1) ? MOUSE_BTN_RIGHT : 0) |
            (packet[0] & (1 << 2) ? MOUSE_BTN_MIDDLE : 0)
    };

    mouse_input_dispatch(&ev);
}
