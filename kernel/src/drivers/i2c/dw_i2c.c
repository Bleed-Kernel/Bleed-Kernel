#include <drivers/i2c/dw_i2c.h>
#include <drivers/ahci/pci.h>
#include <mm/paging.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>
#include <boot/bootloader_interface/generic_bootloader.h>
#include <cpu/cpuid.h>
#include <ACPI/acpi_hpet.h>
#include <sched/scheduler.h>
#include <mm/spinlock.h>

#define MMIO(paddr)  ((uint64_t)(paddr) + g_gbi.hhdm_offset)

static dw_i2c_t dw_buses[DW_I2C_MAX_BUSES];
static int      dw_bus_count = 0;

// the FCH ones arent pci devices at all, but they are always at these addresses
static const uint64_t amd_fch_i2c[] = { 0xFEDC2000, 0xFEDC3000, 0xFEDC4000, 0xFEDC5000 };

// every Intel LPSS I2C controller, straight out of linux's intel-lpss-pci.c.
// being in here is what makes it safe to let go of the reset line,
// save yourself the hastle and steal this
static const lpss_i2c_id_t lpss_i2c_ids[] = {
    { 0x0AAC, 15, 133 },    // broxton
    { 0x1AAC, 15, 133 },    // broxton-p
    { 0x5AAC, 15, 133 },    // apollo lake
    { 0x31AC, 15, 133 },    // gemini lake
    { 0x9D60,  6, 120 },    // skylake / kaby lake LP
    { 0xA160,  3, 120 },    // skylake H
    { 0xA2E0,  4, 120 },    // kaby lake H
    { 0xA3E0,  4, 120 },    // comet lake V
    { 0x9DE8,  4, 216 },    // cannon lake LP
    { 0x9DC5,  2, 216 },
    { 0xA368,  4, 216 },    // cannon lake H
    { 0x02E8,  4, 216 },    // comet lake LP
    { 0x02C5,  2, 216 },
    { 0x06E8,  4, 216 },    // comet lake H
    { 0x34E8,  4, 133 },    // ice lake LP
    { 0x34C5,  2, 133 },
    { 0x4DE8,  4, 133 },    // jasper lake
    { 0x4DC5,  2, 133 },
    { 0x4B78,  4, 100 },    // elkhart lake
    { 0x4B4B,  2, 100 },
    { 0x4B44,  2, 100 },
    { 0xA0E8,  4, 133 },    // tiger lake LP
    { 0xA0C5,  2, 133 },
    { 0xA0D8,  2, 133 },
    { 0x43E8,  4, 133 },    // tiger lake H
    { 0x43AD,  2, 133 },
    { 0x43D8,  1, 133 },
    { 0x51E8,  4, 133 },    // alder lake P
    { 0x51C5,  2, 133 },
    { 0x51D8,  2, 133 },
    { 0x54E8,  4, 133 },    // alder lake N
    { 0x54C5,  2, 133 },
    { 0x7ACC,  4, 133 },    // alder lake S
    { 0x7AFC,  2, 133 },
    { 0x7A4C,  4, 133 },    // raptor lake S
    { 0x7A7C,  2, 133 },
    { 0x7E78,  4, 133 },    // meteor lake P
    { 0x7E50,  2, 133 },
};

// mmio helpers
static inline uint32_t dw_read(uint64_t base, uint32_t offset) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(base + offset);
    return *r;
}

static inline void dw_write(uint64_t base, uint32_t offset, uint32_t val) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(base + offset);
    *r = val;
}

// same hpet timing ohci does
static bool dw_timed_out(uint64_t start, uint64_t spins, uint64_t timeout_ms) {
    uint64_t now = hpet_get_femtoseconds();
    if (now)
        return now - start > timeout_ms * femtosecondsPerMillisecond;
    return spins > timeout_ms * 1000;
}

// wait for (reg & mask) to become set or clear
static bool dw_wait(uint64_t base, uint32_t reg, uint32_t mask,
                    bool want_set, uint64_t timeout_ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        if (!!(dw_read(base, reg) & mask) == want_set)
            return true;
        if (dw_timed_out(start, spins, timeout_ms))
            return false;
        asm volatile("pause");
    }
}

// give the cpu away for a tick while the wire catches up
static void dw_sleep(void) {
    uint64_t      deadline = sched_deadline_ms(1);
    unsigned long flags    = irq_push();
    sched_wait_event(deadline);
    irq_restore(flags);
}

// the target only changes while its switched off, that empties both fifos too
static bool dw_set_target(dw_i2c_t *bus, uint16_t addr) {
    if (bus->target == addr) return true;

    dw_write(bus->base, DW_IC_ENABLE, 0);
    if (!dw_wait(bus->base, DW_IC_ENABLE_STATUS, DW_ENABLE_EN, false, DW_ENABLE_TIMEOUT_MS))
        return false;

    dw_write(bus->base, DW_IC_TAR, addr);
    dw_write(bus->base, DW_IC_ENABLE, DW_ENABLE_EN);
    bus->target = addr;
    return true;
}

static int dw_transfer(dw_i2c_t *bus, uint8_t addr, const uint8_t *out, size_t out_len,
                       uint8_t *in, size_t in_max, bool open) {
    if (!bus || !bus->present || (!out_len && !in_max)) return -1;
    if ((out_len && !out) || (in_max && !in) || (open && in_max < 3)) return -1;

    uint64_t base = bus->base;

    // the last transfer might still be putting its stop on the wire
    if (!dw_wait(base, DW_IC_STATUS, DW_STATUS_MST_ACTIVITY, false, DW_XFER_TIMEOUT_MS))
        bus->target = DW_TARGET_NONE;
    if (!dw_set_target(bus, addr))
        return -2;

    size_t want  = open ? 2 : in_max;
    size_t sent  = 0;
    size_t asked = 0;
    size_t got   = 0;

    dw_read(base, DW_IC_CLR_INTR);

    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        uint32_t raw = dw_read(base, DW_IC_RAW_INTR_STAT);

        if (raw & DW_INTR_TX_ABRT) {
            // the fifo stays held in reset until the abort is read back
            dw_read(base, DW_IC_CLR_TX_ABRT);
            bus->target = DW_TARGET_NONE;
            return -1;
        }
        if (sent == out_len && !open && got == want && (raw & DW_INTR_STOP_DET))
            return (int)got;

        uint32_t status = dw_read(base, DW_IC_STATUS);
        bool     moved  = false;

        if ((status & DW_STATUS_RFNE) && got < want) {
            in[got++] = (uint8_t)dw_read(base, DW_IC_DATA_CMD);
            moved = true;

            if (open && got == 2) {
                // one more byte either way, the stop has to ride on something
                size_t len = in[0] | ((size_t)in[1] << 8);
                want = (len > 2 && len <= in_max) ? len : 3;
                open = false;
            }
        }

        // never ask for more than the rx fifo can hold or it overruns while we arent looking
        bool more_out = sent < out_len;
        bool more_in  = asked < want && asked - got < bus->rx_depth;
        if ((status & DW_STATUS_TFNF) && (more_out || more_in)) {
            uint32_t cmd;
            if (more_out) {
                cmd = out[sent++];
            } else {
                cmd = DW_CMD_READ;
                if (asked++ == 0 && out_len)
                    cmd |= DW_CMD_RESTART;
            }

            if (sent == out_len && asked == want && !open)
                cmd |= DW_CMD_STOP;
            dw_write(base, DW_IC_DATA_CMD, cmd);
            moved = true;
        }

        if (moved) {
            start = hpet_get_femtoseconds();
            spins = 0;
        } else if (dw_timed_out(start, spins, DW_XFER_TIMEOUT_MS)) {
            bus->target = DW_TARGET_NONE;
            return -2;
        } else if (asked - got >= DW_SLEEP_BYTES && start && get_current_task()) {
            // nothing to sleep on during boot, or without the hpet to wake us back up
            dw_sleep();
        } else {
            asm volatile("pause");
        }
    }
}

int dw_i2c_xfer(dw_i2c_t *bus, uint8_t addr, const void *out, size_t out_len,
                void *in, size_t in_len) {
    return dw_transfer(bus, addr, out, out_len, in, in_len, false);
}

int dw_i2c_read_prefixed(dw_i2c_t *bus, uint8_t addr, void *in, size_t max) {
    return dw_transfer(bus, addr, NULL, 0, in, max, true);
}

// clock_mhz of 0 = we dont know what this is, look but dont touch until its sure
static bool dw_bus_init(dw_i2c_t *bus, uint64_t phys, bool lpss, uint16_t clock_mhz) {
    if (paging_map_mmio(phys, PAGE_SIZE_4K) < 0)
        return false;
    uint64_t base = MMIO(phys);

    uint32_t type = dw_read(base, DW_IC_COMP_TYPE);

    // firmware that never used the bus leaves it held in reset and it wont answer
    if (type != DW_COMP_TYPE_VALUE && lpss && clock_mhz) {
        dw_write(base, LPSS_PRIV_RESETS, LPSS_RESETS_FUNC | LPSS_RESETS_IDMA);
        type = dw_read(base, DW_IC_COMP_TYPE);
    }

    if (type != DW_COMP_TYPE_VALUE) {
        // all 0s or all 1s is just nothing being there
        if (type && type != 0xFFFFFFFF)
            serial_printf(LOG_INFO "dwi2c: 0x%llx isnt DesignWare (COMP_TYPE=0x%08x)\n", phys, type);
        return false;
    }

    dw_write(base, DW_IC_ENABLE, 0);
    if (!dw_wait(base, DW_IC_ENABLE_STATUS, DW_ENABLE_EN, false, DW_ENABLE_TIMEOUT_MS)) {
        serial_printf(LOG_WARN "dwi2c: controller at 0x%llx wont switch off\n", phys);
        return false;
    }

    dw_write(base, DW_IC_CON, DW_CON_MASTER | DW_CON_SPEED_FAST
                              | DW_CON_RESTART_EN | DW_CON_SLAVE_DISABLE);
    dw_write(base, DW_IC_INTR_MASK, 0);     // everything is polled

    // with no idea of the clock whatever the firmware worked out beats our guess
    bool tuned = dw_read(base, DW_IC_FS_SCL_HCNT) && dw_read(base, DW_IC_FS_SCL_LCNT);
    if (clock_mhz || !tuned) {
        uint16_t mhz = clock_mhz ? clock_mhz : DW_CLOCK_UNKNOWN_MHZ;
        dw_write(base, DW_IC_FS_SCL_HCNT, DW_FS_SCL_HCNT(mhz));
        dw_write(base, DW_IC_FS_SCL_LCNT, DW_FS_SCL_LCNT(mhz));
    }

    // the hold time is tuned per board, only fill it in if nobody has
    if (!(dw_read(base, DW_IC_SDA_HOLD) & 0xFFFF))
        dw_write(base, DW_IC_SDA_HOLD, DW_SDA_HOLD(clock_mhz ? clock_mhz : DW_CLOCK_UNKNOWN_MHZ));

    memset(bus, 0, sizeof(*bus));
    bus->base   = base;
    bus->target = DW_TARGET_NONE;

    // depth - 1 lives in bits 8-15, all 0 means the block was built without the register
    uint32_t param = dw_read(base, DW_IC_COMP_PARAM_1);
    bus->rx_depth  = param ? ((param >> 8) & 0xFF) + 1 : DW_RX_DEPTH_FALLBACK;

    bus->present = true;
    serial_printf(LOG_OK "dwi2c: bus %d at 0x%llx, %uMHz clock, rx fifo %u deep\n",
                  dw_bus_count, phys, clock_mhz, bus->rx_depth);
    return true;
}

// LPSS powers its controllers down to D3 when nothing is using them
static void dw_pci_wake(const pci_device_t *pci_dev) {
    uint8_t bus = pci_dev->bus, dev = pci_dev->device, func = pci_dev->function;

    pci_write16(bus, dev, func, PCI_COMMAND,
                pci_read16(bus, dev, func, PCI_COMMAND) | PCI_CMD_MEMORY);

    if (!(pci_read16(bus, dev, func, PCI_STATUS) & PCI_STATUS_CAP_LIST))
        return;

    uint8_t cap = pci_read8(bus, dev, func, PCI_CAP_POINTER) & 0xFC;
    for (int guard = 0; cap >= 0x40 && guard < 48; guard++) {
        if (pci_read8(bus, dev, func, cap) == PCI_CAP_ID_PM) {
            uint16_t ctrl = pci_read16(bus, dev, func, cap + PCI_PM_CTRL);
            if (ctrl & PCI_PM_CTRL_STATE_MASK) {
                pci_write16(bus, dev, func, cap + PCI_PM_CTRL,
                            ctrl & ~PCI_PM_CTRL_STATE_MASK);
                wait_ms(10);    // 10ms is what the spec allows for
            }
            return;
        }
        cap = pci_read8(bus, dev, func, cap + 1) & 0xFC;
    }
}

static bool cpu_is_amd(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    return ebx == 0x68747541 && edx == 0x69746E65 && ecx == 0x444D4163;   // AuthenticAMD
}

dw_i2c_t *dw_i2c_get_bus(int index) {
    if (index < 0 || index >= dw_bus_count) return NULL;
    return &dw_buses[index];
}

static uint16_t lpss_i2c_clock(uint16_t device_id) {
    for (size_t i = 0; i < sizeof(lpss_i2c_ids) / sizeof(lpss_i2c_ids[0]); i++) {
        const lpss_i2c_id_t *id = &lpss_i2c_ids[i];
        if (device_id >= id->first && device_id < id->first + id->count)
            return id->clock_mhz;
    }
    return 0;
}

static void dw_pci_scan(uint8_t class_code) {
    pci_device_t pci_dev;
    for (int n = 0; dw_bus_count < DW_I2C_MAX_BUSES &&
                    pci_find_device_nth(class_code, PCI_SUBCLASS_SERIAL_OTHER,
                                        PCI_PROGIF_NONE, n, &pci_dev); n++) {
        serial_printf(LOG_INFO "dwi2c: candidate PCI %02x:%02x.%x vendor=0x%04x device=0x%04x "
                      "bar0=0x%08x bar1=0x%08x\n",
                      pci_dev.bus, pci_dev.device, pci_dev.function,
                      pci_dev.vendor_id, pci_dev.device_id, pci_dev.bar[0], pci_dev.bar[1]);

        // the spi flash controller has the same class, dont go poking at that
        if (pci_dev.vendor_id != PCI_VENDOR_INTEL || pci_dev.device == PCI_DEVICE_PCH_LPC)
            continue;

        dw_pci_wake(&pci_dev);

        uint64_t phys = pci_dev.bar[0] & ~0xFull;
        if ((pci_dev.bar[0] & 0x6) == 0x4)      // 64bit BAR
            phys |= (uint64_t)pci_dev.bar[1] << 32;
        if (phys == 0)
            continue;

        if (dw_bus_init(&dw_buses[dw_bus_count], phys, true, lpss_i2c_clock(pci_dev.device_id))) {
            serial_printf(LOG_INFO "dwi2c: bus %d is PCI %02x:%02x.%x "
                          "vendor=0x%04x device=0x%04x\n", dw_bus_count,
                          pci_dev.bus, pci_dev.device, pci_dev.function,
                          pci_dev.vendor_id, pci_dev.device_id);
            dw_bus_count++;
        }
    }
}

void dw_i2c_init(void) {
    memset(dw_buses, 0, sizeof(dw_buses));

    // LPSS is filed under serial bus on newer chipsets and signal processing on
    // skylake and apollo lake, other things live in both
    dw_pci_scan(PCI_CLASS_SERIAL_BUS);
    dw_pci_scan(PCI_CLASS_SIGNAL);

    if (cpu_is_amd()) {
        for (size_t i = 0; i < sizeof(amd_fch_i2c) / sizeof(amd_fch_i2c[0]) &&
                           dw_bus_count < DW_I2C_MAX_BUSES; i++) {
            if (dw_bus_init(&dw_buses[dw_bus_count], amd_fch_i2c[i], false, DW_CLOCK_AMD_MHZ))
                dw_bus_count++;
        }
    }

    if (dw_bus_count == 0)
        serial_printf(LOG_INFO "dwi2c: no DesignWare I2C controller found\n");
}
