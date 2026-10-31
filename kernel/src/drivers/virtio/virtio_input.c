#include <drivers/virtio/virtio_input.h>
#include <drivers/virtio/virtio_pci.h>
#include <drivers/virtio/virtio_queue.h>
#include <drivers/ahci/pci.h>
#include <drivers/serial/serial.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static virtio_input_t inputs[VIRTIO_INPUT_MAX];
static int            input_count = 0;

static const char *kind_names[] = { "something else", "mouse", "tablet", "touchscreen" };

// ask the config a question, returns how many bytes the answer is
static uint8_t virtio_input_query(virtio_input_t *in, uint8_t select, uint8_t subsel) {
    virtio_write8(in->vp.device, VIRTIO_INPUT_CFG_SELECT, select);
    virtio_write8(in->vp.device, VIRTIO_INPUT_CFG_SUBSEL, subsel);
    return virtio_read8(in->vp.device, VIRTIO_INPUT_CFG_SIZE);
}

static bool virtio_input_has(virtio_input_t *in, uint8_t type, uint16_t code) {
    uint8_t size = virtio_input_query(in, VIRTIO_INPUT_CFG_EV_BITS, type);
    if (code / 8 >= size) return false;
    return virtio_read8(in->vp.device, VIRTIO_INPUT_CFG_DATA + code / 8) & (1u << (code % 8));
}

static virtio_input_axis_t virtio_input_abs(virtio_input_t *in, uint8_t code) {
    virtio_input_axis_t axis = { 0, 0 };
    if (virtio_input_query(in, VIRTIO_INPUT_CFG_ABS_INFO, code) >= 8) {
        axis.min = (int32_t)virtio_read32(in->vp.device, VIRTIO_INPUT_CFG_DATA);
        axis.max = (int32_t)virtio_read32(in->vp.device, VIRTIO_INPUT_CFG_DATA + 4);
    }
    return axis;
}

static void virtio_input_identify(virtio_input_t *in) {
    uint8_t len = virtio_input_query(in, VIRTIO_INPUT_CFG_ID_NAME, 0);
    if (len >= sizeof(in->name))
        len = sizeof(in->name) - 1;
    for (uint8_t i = 0; i < len; i++)
        in->name[i] = (char)virtio_read8(in->vp.device, VIRTIO_INPUT_CFG_DATA + i);
    in->name[len] = '\0';

    if (virtio_input_has(in, EV_ABS, ABS_MT_POSITION_X)) {
        in->kind   = VIRTIO_INPUT_TOUCH;
        in->x_axis = virtio_input_abs(in, ABS_MT_POSITION_X);
        in->y_axis = virtio_input_abs(in, ABS_MT_POSITION_Y);
    } else if (virtio_input_has(in, EV_ABS, ABS_X)) {
        in->kind   = VIRTIO_INPUT_TABLET;
        in->x_axis = virtio_input_abs(in, ABS_X);
        in->y_axis = virtio_input_abs(in, ABS_Y);
    } else if (virtio_input_has(in, EV_REL, REL_X)) {
        in->kind = VIRTIO_INPUT_MOUSE;
    } else {
        in->kind = VIRTIO_INPUT_OTHER;
    }
}

static bool virtio_input_start(virtio_input_t *in, const pci_device_t *pci) {
    memset(in, 0, sizeof(*in));
    if (!virtio_pci_init(&in->vp, pci))
        return false;

    virtio_input_identify(in);
    serial_printf(LOG_INFO "virtio: input \"%s\" is a %s, x %d-%d y %d-%d\n", in->name,
                  kind_names[in->kind], in->x_axis.min, in->x_axis.max, in->y_axis.min, in->y_axis.max);

    // keyboards come in on the ps/2 or usb ones for now
    if (in->kind == VIRTIO_INPUT_OTHER) {
        virtio_pci_fail(&in->vp);
        return false;
    }

    // queue 0 is events from the device, 1 is status going to it which we dont send
    if (!virtq_setup(&in->vp, &in->eventq, 0)) {
        virtio_pci_fail(&in->vp);
        return false;
    }

    in->events_phys = pmm_alloc_pages(1);
    if (!in->events_phys) {
        virtio_pci_fail(&in->vp);
        return false;
    }
    in->events = paddr_to_vaddr(in->events_phys);
    memset(in->events, 0, PAGE_SIZE);

    for (int s = 0; s < VIRTIO_INPUT_SLOTS; s++)
        in->slots[s].id = -1;
    for (uint16_t i = 0; i < in->eventq.size; i++)
        virtq_give(&in->eventq, i, in->events_phys + i * sizeof(virtio_input_event_t),
                   sizeof(virtio_input_event_t));

    virtio_pci_ready(&in->vp);
    virtq_kick(&in->eventq);
    in->used = true;

    serial_printf(LOG_OK "virtio: %s ready\n", kind_names[in->kind]);
    return true;
}

void virtio_input_init(void) {
    pci_device_t pci;
    for (int n = 0; pci_find_device_nth(PCI_CLASS_INPUT, PCI_SUBCLASS_INPUT_OTHER, 0, n, &pci); n++) {
        if (pci.vendor_id != VIRTIO_PCI_VENDOR ||
            pci.device_id != VIRTIO_PCI_DEVICE_BASE + VIRTIO_ID_INPUT) continue;
        if (input_count >= VIRTIO_INPUT_MAX) {
            serial_printf(LOG_WARN "virtio: more than %u input devices, ignoring the rest\n", VIRTIO_INPUT_MAX);
            break;
        }

        serial_printf(LOG_INFO "virtio: input device at %02x:%02x.%u\n", pci.bus, pci.device, pci.function);
        if (virtio_input_start(&inputs[input_count], &pci))
            input_count++;
    }
}

void virtio_input_poll(void) {
    for (int i = 0; i < input_count; i++) {
        virtio_input_t *in = &inputs[i];
        if (!in->used) continue;

        uint16_t desc;
        uint32_t len;
        bool     gave = false;
        while (virtq_take(&in->eventq, &desc, &len)) {
            if (desc < in->eventq.size && len >= sizeof(virtio_input_event_t))
                virtio_input_event(in, &in->events[desc]);

            // straight back to the device for the next one
            virtq_give(&in->eventq, desc, in->events_phys + desc * sizeof(virtio_input_event_t),
                       sizeof(virtio_input_event_t));
            gave = true;
        }
        if (gave)
            virtq_kick(&in->eventq);
    }
}
