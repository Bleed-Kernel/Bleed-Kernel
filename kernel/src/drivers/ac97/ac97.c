#include <drivers/ac97/ac97.h>
#include <drivers/ahci/pci.h>
#include <drivers/serial/serial.h>
#include <devices/type/audio_device.h>
#include <ACPI/acpi_hpet.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <cpu/io.h>
#include <cpu/cpuid.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

typedef struct {
    uint16_t          nam;
    uint16_t          nabm;
    bool              vra;
    bool              qemu_volume;
    uint8_t           master_max;
    uint8_t           last_civ;
    uint32_t          quiet_ticks;
    uint32_t          stuck_ticks;
    bool              told_moving;
    bool              told_stuck;
    uint32_t          rate;
    paddr_t           bdl_phys;
    ac97_bdl_entry_t *bdl;
    audio_device_t   *audio;
} ac97_t;

static ac97_t ac97;

// port helpers
static inline uint16_t nam_read(ac97_t *c, uint8_t reg)              { return inw(c->nam + reg); }
static inline void     nam_write(ac97_t *c, uint8_t reg, uint16_t v) { outw(c->nam + reg, v); }
static inline uint8_t  nabm_read8(ac97_t *c, uint8_t reg)            { return inb(c->nabm + reg); }
static inline uint16_t nabm_read16(ac97_t *c, uint8_t reg)           { return inw(c->nabm + reg); }
static inline uint32_t nabm_read32(ac97_t *c, uint8_t reg)           { return inl(c->nabm + reg); }
static inline void     nabm_write8(ac97_t *c, uint8_t reg, uint8_t v)   { outb(c->nabm + reg, v); }
static inline void     nabm_write16(ac97_t *c, uint8_t reg, uint16_t v) { outw(c->nabm + reg, v); }
static inline void     nabm_write32(ac97_t *c, uint8_t reg, uint32_t v) { outl(c->nabm + reg, v); }

static bool ac97_wait(ac97_t *c, uint8_t reg, uint32_t mask, bool set, uint32_t ms) {
    uint64_t deadline = hpet_get_femtoseconds() + (uint64_t)ms * 1000000000000ULL;
    while (hpet_get_femtoseconds() < deadline) {
        bool now = (nabm_read32(c, reg) & mask) != 0;
        if (now == set)
            return true;
        asm volatile("pause");
    }
    return false;
}

static bool ac97_box_reset(ac97_t *c) {
    nabm_write8(c, AC97_PO_CR, 0);
    nabm_write8(c, AC97_PO_CR, AC97_CR_RR);
    for (int i = 0; i < 100000; i++) {
        if (!(nabm_read8(c, AC97_PO_CR) & AC97_CR_RR))
            return true;
        asm volatile("pause");
    }
    return false;
}

static int ac97_start(void *priv) {
    ac97_t *c = priv;
    if (!ac97_box_reset(c))
        return -EIO;

    // LVI one behind CIV the whole time
    nabm_write32(c, AC97_PO_BDBAR, (uint32_t)c->bdl_phys);
    nabm_write8(c, AC97_PO_LVI, AC97_BDL_ENTRIES - 1);
    nabm_write16(c, AC97_PO_SR, AC97_SR_CLEAR);
    nabm_write8(c, AC97_PO_CR, AC97_CR_RPBM);
    c->last_civ    = 0;
    c->quiet_ticks = 0;
    c->stuck_ticks = 0;
    return 0;
}

static void ac97_stop(void *priv) {
    ac97_t *c = priv;
    nabm_write8(c, AC97_PO_CR, 0);
    for (int i = 0; i < 100000 && !(nabm_read16(c, AC97_PO_SR) & AC97_SR_DCH); i++)
        asm volatile("pause");
    ac97_box_reset(c);
}

static uint32_t ac97_hw_offset(void *priv) {
    ac97_t *c = priv;

    // CIV and PICB cant be read together, go again if it moved buffers between them
    uint8_t  civ, again;
    uint16_t picb;
    do {
        civ   = nabm_read8(c, AC97_PO_CIV) % AC97_BDL_ENTRIES;
        picb  = nabm_read16(c, AC97_PO_PICB);
        again = nabm_read8(c, AC97_PO_CIV) % AC97_BDL_ENTRIES;
    } while (civ != again);

    uint32_t left = (uint32_t)picb * 2;
    if (left > AC97_PERIOD_BYTES)
        left = AC97_PERIOD_BYTES;
    return civ * AC97_PERIOD_BYTES + (AC97_PERIOD_BYTES - left);
}

static int ac97_set_rate(void *priv, uint32_t rate) {
    ac97_t *c = priv;
    if (!c->vra)
        return AC97_RATE_DEFAULT;
    if (rate < 8000 || rate > 48000)
        return -EINVAL;

    nam_write(c, AC97_NAM_FRONT_RATE, (uint16_t)rate);
    c->rate = nam_read(c, AC97_NAM_FRONT_RATE);
    return (int)c->rate;
}

// the slider as an amplitude out of 65536
static uint32_t ac97_amp(uint8_t percent) {
    uint32_t amp = 65536;
    for (int i = percent; i < 100; i++)
        amp = (amp * AC97_VOL_STEP_Q16) >> 16;
    return amp;
}

// real hardware, handled in 1.5DB steps
static void ac97_steps_real(ac97_t *c, uint8_t percent, uint8_t *master, uint8_t *pcm) {
    uint32_t steps = ((100 - percent) * AC97_VOL_CENTI_DB + AC97_STEP_CENTI_DB / 2) / AC97_STEP_CENTI_DB;
    uint32_t m     = steps < c->master_max ? steps : c->master_max;
    uint32_t p     = AC97_PCM_0DB_STEP + (steps - m);
    *master = (uint8_t)m;
    *pcm    = (uint8_t)(p > AC97_PCM_MAX_STEP ? AC97_PCM_MAX_STEP : p);
}

// qemu has to be handled a bit different, whatever pair lands closest to the amplitude
static void ac97_steps_qemu(uint8_t percent, uint8_t *master, uint8_t *pcm) {
    uint32_t want = (ac97_amp(percent) * 255 + 32768) >> 16;
    if (!want)
        want = 1;

    uint32_t best = 0xFFFFFFFF;
    for (uint32_t p = 0; p <= AC97_QEMU_PCM_MAX; p++) {
        for (uint32_t m = 0; m <= AC97_QEMU_MASTER_MAX; m++) {
            uint32_t got  = (255 * (AC97_QEMU_MASTER_MAX - m) / AC97_QEMU_MASTER_MAX) *
                            (255 * (AC97_QEMU_PCM_MAX - p) / AC97_QEMU_PCM_MAX) / 255;
            uint32_t miss = got > want ? got - want : want - got;
            if (miss < best) {
                best    = miss;
                *master = (uint8_t)m;
                *pcm    = (uint8_t)p;
            }
        }
    }
}

static int ac97_set_volume(void *priv, const struct audio_volume *vol) {
    ac97_t *c = priv;
    uint8_t ml, mr, pl, pr;

    if (c->qemu_volume) {
        ac97_steps_qemu(vol->left, &ml, &pl);
        ac97_steps_qemu(vol->right, &mr, &pr);
    } else {
        ac97_steps_real(c, vol->left, &ml, &pl);
        ac97_steps_real(c, vol->right, &mr, &pr);
    }

    uint16_t master = (uint16_t)((ml << 8) | mr);
    if (vol->mute || (!vol->left && !vol->right))
        master |= AC97_VOL_MUTE;

    nam_write(c, AC97_NAM_MASTER, master);
    nam_write(c, AC97_NAM_HEADPHONE, master);
    nam_write(c, AC97_NAM_PCM_OUT, (uint16_t)((pl << 8) | pr));
    return 0;
}

static bool ac97_is_qemu(ac97_t *c) {
    uint32_t a, b, cx, d;
    cpuid(1, &a, &b, &cx, &d);
    if (!(cx & (1u << 31)))
        return false;

    char vendor[13] = {0};
    cpuid(0x40000000, &a, &b, &cx, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &cx, 4);
    memcpy(vendor + 8, &d, 4);
    if (strcmp(vendor, "TCGTCGTCGTCG") && strncmp(vendor, "KVMKVMKVM", 9))
        return false;

    uint32_t codec = ((uint32_t)nam_read(c, AC97_NAM_VENDOR1) << 16) | nam_read(c, AC97_NAM_VENDOR2);
    return codec == AC97_QEMU_VENDOR;
}

static const audio_driver_t ac97_driver = {
    .name       = "ac97",
    .start      = ac97_start,
    .stop       = ac97_stop,
    .hw_offset  = ac97_hw_offset,
    .set_rate   = ac97_set_rate,
    .set_volume = ac97_set_volume,
};

// every port access is a trip out to the hypervisor under emulation
void ac97_poll(void) {
    ac97_t *c = &ac97;
    if (!c->audio || !c->audio->running)
        return;

    if (c->quiet_ticks) {
        c->quiet_ticks--;
        return;
    }

    uint8_t civ = nabm_read8(c, AC97_PO_CIV) % AC97_BDL_ENTRIES;
    if (civ == c->last_civ) {
        if (++c->stuck_ticks == AC97_STUCK_TICKS && !c->told_stuck) {
            c->told_stuck = true;
        }
        return;
    }

    if (!c->told_moving) {
        c->told_moving = true;
    }
    c->stuck_ticks = 0;
    c->last_civ    = civ;
    c->quiet_ticks = AC97_PERIOD_FRAMES * 1000 / c->rate - 1;
    audio_device_advance(c->audio, civ);
    if (!c->audio->running)
        return;

    nabm_write8(c, AC97_PO_LVI, (uint8_t)((civ + AC97_BDL_ENTRIES - 1) % AC97_BDL_ENTRIES));
}

static bool ac97_codec_up(ac97_t *c) {
    // cold reset, firmware may have left it in any state
    nabm_write32(c, AC97_GLOB_CNT, 0);
    uint64_t until = hpet_get_femtoseconds() + AC97_RESET_WAIT_MS * 1000000000000ULL;
    while (hpet_get_femtoseconds() < until)
        asm volatile("pause");
    nabm_write32(c, AC97_GLOB_CNT, AC97_GLOB_COLD);

    if (!ac97_wait(c, AC97_GLOB_STA, AC97_STA_PCR, true, AC97_CODEC_WAIT_MS))
        return false;

    nam_write(c, AC97_NAM_RESET, 0);
    serial_printf(LOG_INFO "ac97: codec %04x:%04x, ext id 0x%x, glob sta 0x%x\n", nam_read(c, AC97_NAM_VENDOR1),
                  nam_read(c, AC97_NAM_VENDOR2), nam_read(c, AC97_NAM_EXT_ID), nabm_read32(c, AC97_GLOB_STA));

    c->rate = AC97_RATE_DEFAULT;
    if (nam_read(c, AC97_NAM_EXT_ID) & AC97_EXT_VRA) {
        nam_write(c, AC97_NAM_EXT_CTRL, nam_read(c, AC97_NAM_EXT_CTRL) | AC97_EXT_VRA);
        nam_write(c, AC97_NAM_FRONT_RATE, AC97_RATE_DEFAULT);
        c->vra = nam_read(c, AC97_NAM_FRONT_RATE) == AC97_RATE_DEFAULT;
    }

    // a 5 bit master reads back 
    nam_write(c, AC97_NAM_MASTER, AC97_VOL_PROBE);
    c->master_max  = (nam_read(c, AC97_NAM_MASTER) & 0x3F) == 0x20 ? 63 : 31;
    c->qemu_volume = ac97_is_qemu(c);

    struct audio_volume full = { .left = 100, .right = 100 };
    ac97_set_volume(c, &full);
    serial_printf(LOG_INFO "ac97: %u Hz%s, %u bit master, %s volume, master 0x%x pcm 0x%x\n", c->rate,
                  c->vra ? " variable" : "", c->master_max == 63 ? 6 : 5, c->qemu_volume ? "qemu" : "codec",
                  nam_read(c, AC97_NAM_MASTER), nam_read(c, AC97_NAM_PCM_OUT));
    return true;
}

static bool ac97_dma_setup(ac97_t *c, paddr_t *ring_phys) {
    size_t ring_pages = (AC97_BDL_ENTRIES * AC97_PERIOD_BYTES) / PAGE_SIZE;

    c->bdl_phys = pmm_alloc_pages(1);
    *ring_phys  = pmm_alloc_pages(ring_pages);
    if (!c->bdl_phys || !*ring_phys) {
        serial_printf(LOG_ERROR "ac97: OOM for the dma ring\n");
        return false;
    }

    if (((uint64_t)c->bdl_phys >> 32) || (((uint64_t)*ring_phys + ring_pages * PAGE_SIZE - 1) >> 32)) {
        serial_printf(LOG_ERROR "ac97: dma memory landed above 4G\n");
        pmm_free_pages(c->bdl_phys, 1);
        pmm_free_pages(*ring_phys, ring_pages);
        return false;
    }

    c->bdl = paddr_to_vaddr(c->bdl_phys);
    memset(c->bdl, 0, PAGE_SIZE);
    memset(paddr_to_vaddr(*ring_phys), 0, ring_pages * PAGE_SIZE);

    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        c->bdl[i].addr    = (uint32_t)(*ring_phys + i * AC97_PERIOD_BYTES);
        c->bdl[i].samples = AC97_PERIOD_BYTES / 2;
        c->bdl[i].flags   = 0;
    }
    return true;
}

static bool ac97_ich_needs_iose(const pci_device_t *pci) {
    static const uint16_t ich4_up[] = { 0x24C5, 0x24D5, 0x25A6, 0x266E, 0x27DE };
    if (pci->vendor_id != PCI_VENDOR_INTEL)
        return false;
    for (size_t i = 0; i < sizeof(ich4_up) / sizeof(ich4_up[0]); i++) {
        if (pci->device_id == ich4_up[i])
            return true;
    }
    return false;
}

void ac97_init(void) {
    pci_device_t pci;
    if (!pci_find_device(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, PCI_PROGIF_AC97, &pci))
        return;

    ac97_t *c = &ac97;
    memset(c, 0, sizeof(*c));

    if (ac97_ich_needs_iose(&pci)) {
        uint16_t ich = pci_read16(pci.bus, pci.device, pci.function, AC97_PCI_CFG_ICH);
        pci_write16(pci.bus, pci.device, pci.function, AC97_PCI_CFG_ICH, ich | (AC97_ICH_IOSE << 8));
        pci.bar[0] = pci_read32(pci.bus, pci.device, pci.function, PCI_BAR0);
        pci.bar[1] = pci_read32(pci.bus, pci.device, pci.function, PCI_BAR1);
    }

    if (!(pci.bar[0] & 1) || !(pci.bar[1] & 1)) {
        serial_printf(LOG_WARN "ac97: %04x:%04x has no io bars, skipping it\n", pci.vendor_id, pci.device_id);
        return;
    }
    c->nam  = (uint16_t)(pci.bar[0] & ~3u);
    c->nabm = (uint16_t)(pci.bar[1] & ~3u);

    uint16_t cmd = pci_read16(pci.bus, pci.device, pci.function, PCI_COMMAND);
    pci_write16(pci.bus, pci.device, pci.function, PCI_COMMAND, cmd | PCI_CMD_IO | PCI_CMD_BUSMASTER);

    serial_printf(LOG_INFO "ac97: %04x:%04x at %02x:%02x.%x, nam 0x%x nabm 0x%x\n", pci.vendor_id,
                  pci.device_id, pci.bus, pci.device, pci.function, c->nam, c->nabm);

    if (!ac97_codec_up(c)) {
        serial_printf(LOG_ERROR "ac97: codec never came ready\n");
        return;
    }

    paddr_t ring_phys = 0;
    if (!ac97_dma_setup(c, &ring_phys))
        return;
    ac97_box_reset(c);

    c->audio = audio_device_register("audio0", &ac97_driver, c, ring_phys, AC97_PERIOD_BYTES,
                                     AC97_BDL_ENTRIES, AC97_RATE_DEFAULT, 2);
    if (!c->audio)
        serial_printf(LOG_ERROR "ac97: couldnt register /dev/audio0\n");
}
