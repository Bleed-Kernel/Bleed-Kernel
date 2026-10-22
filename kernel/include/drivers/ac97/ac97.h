#pragma once

#include <stdint.h>
#include <stdbool.h>

// PCI identifiers, multimedia audio
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_SUBCLASS_AUDIO      0x01
#define PCI_PROGIF_AC97         0x00

#define AC97_PCI_CFG_ICH        0x40
#define AC97_ICH_IOSE           0x01

// mixer
#define AC97_NAM_RESET          0x00
#define AC97_NAM_MASTER         0x02
#define AC97_NAM_HEADPHONE      0x04
#define AC97_NAM_PCM_OUT        0x18
#define AC97_NAM_EXT_ID         0x28
#define AC97_NAM_EXT_CTRL       0x2A
#define AC97_NAM_FRONT_RATE     0x2C
#define AC97_NAM_VENDOR1        0x7C
#define AC97_NAM_VENDOR2        0x7E

#define AC97_EXT_VRA            (1u << 0)   // variable rate, without it the codec is 48k only
#define AC97_PCM_0DB            0x0808
#define AC97_VOL_MUTE           (1u << 15)
#define AC97_VOL_PROBE          0x2020      // bit 5 sticks on a codec with a 6 bit master, a 5 bit one reads 0x1F
#define AC97_PCM_0DB_STEP       8           // pcm out is +12dB at 0, 1.5dB a step down from there
#define AC97_PCM_MAX_STEP       31
#define AC97_STEP_CENTI_DB      150         // a real codec step
#define AC97_VOL_CENTI_DB       48          // a percent of the slider, 48dB from 100 down to 0
#define AC97_VOL_STEP_Q16       62013
#define AC97_QEMU_VENDOR        0x83847600  // the codec qemu pretends to be

// qemus ac97 treats the volume registers as linear amplitude out of 255, real codecs are 1.5dB steps
#define AC97_QEMU_MASTER_MAX    63
#define AC97_QEMU_PCM_MAX       31

// bus master 
#define AC97_PO_BDBAR           0x10
#define AC97_PO_CIV             0x14
#define AC97_PO_LVI             0x15
#define AC97_PO_SR              0x16
#define AC97_PO_PICB            0x18    // samples left in the current buffer
#define AC97_PO_CR              0x1B
#define AC97_GLOB_CNT           0x2C
#define AC97_GLOB_STA           0x30

#define AC97_CR_RPBM            (1u << 0)   // run
#define AC97_CR_RR              (1u << 1)   // reset the box
#define AC97_SR_DCH             (1u << 0)   // halted
#define AC97_SR_CLEAR           0x1C

#define AC97_GLOB_COLD          (1u << 1)
#define AC97_STA_PCR            (1u << 8)

#define AC97_BDL_ENTRIES        32
#define AC97_PERIOD_FRAMES      256 
#define AC97_FRAME_BYTES        4
#define AC97_PERIOD_BYTES       (AC97_PERIOD_FRAMES * AC97_FRAME_BYTES)
#define AC97_RATE_DEFAULT       48000
#define AC97_CODEC_WAIT_MS      500
#define AC97_RESET_WAIT_MS      10

typedef struct {
    uint32_t addr;
    uint16_t samples;       // 16 bit samples, not frames
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

/// @brief find an AC97 controller, bring the codec up and register /dev/audio0
void ac97_init(void);

// off the timer tick, keeps the dma fed 
void ac97_poll(void);
