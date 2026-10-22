#pragma once

#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/spinlock.h>
#include <sched/wait_queue.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AUDIO_FORMAT_S16LE  1

#define AUDIO_IOC_GET_INFO   0x5101  // struct audio_info out
#define AUDIO_IOC_ACQUIRE    0x5102  // struct audio_map out, maps the ring into the caller
#define AUDIO_IOC_RELEASE    0x5103
#define AUDIO_IOC_START      0x5104
#define AUDIO_IOC_STOP       0x5105  // stops and drops anything queued
#define AUDIO_IOC_GET_POS    0x5106  // audio_pos
#define AUDIO_IOC_COMMIT     0x5107  // uint64_t bytes in, how much was written at write_pos
#define AUDIO_IOC_SET_RATE   0x5108  // uint32_t in/out, stopped only, out is what the codec took
#define AUDIO_IOC_SET_VOLUME 0x5109  // struct audio_volume in, anyone can, its the cards own volume
#define AUDIO_IOC_GET_OWNER  0x510A  // pid
#define AUDIO_IOC_WAIT       0x510B  // sleep till the hardware finishes the period its on
#define AUDIO_IOC_GET_VOLUME 0x510C  // struct audio_volume out, what SET_VOLUME last set

#define AUDIO_WAIT_MS        100     // a WAIT never sleeps longer than this

struct audio_info {
    uint32_t rate;
    uint32_t channels;
    uint32_t format;        // AUDIO_FORMAT_*
    uint32_t frame_bytes;
    uint32_t period_bytes;  // the hardware moves on in steps of this
    uint32_t periods;
    uint64_t ring_bytes;
};

struct audio_map {
    uint64_t addr;          // ring in the callers address space
    uint64_t ring_bytes;
};

// write_pos, played and hw_pos are bytes since START and only go up, ring offset is pos % ring_bytes
struct audio_pos {
    uint64_t write_pos;
    uint64_t writable;
    uint64_t played;
    uint64_t hw_pos;
    uint64_t underruns;
    uint32_t running;
    uint32_t _pad;
};

struct audio_volume {
    uint8_t left;
    uint8_t right;
    uint8_t mute;
    uint8_t _pad;
};

// what a sound card driver gives the audio device
typedef struct audio_driver {
    const char *name;
    int      (*start)(void *priv);
    void     (*stop)(void *priv);
    uint32_t (*hw_offset)(void *priv);
    int      (*set_rate)(void *priv, uint32_t rate);
    int      (*set_volume)(void *priv, const struct audio_volume *vol); 
} audio_driver_t;

typedef struct audio_device {
    INode_t     device;
    spinlock_t  lock;
    const audio_driver_t *driver;
    void       *priv;

    paddr_t     ring_phys;      // contiguous, the driver points its dma at it
    uint8_t    *ring;
    size_t      ring_bytes;
    size_t      period_bytes;
    uint32_t    periods;
    uint32_t    rate;
    uint32_t    channels;

    bool        running;
    uint64_t    played;         // periods done and zeroed, only the service moves it
    uint64_t    write_pos;
    uint64_t    underruns;

    struct audio_volume volume;
    uint64_t    owner_pid;
    uint8_t     owner_prio;     // what the owner was before it got the audio boost
    uintptr_t   owner_map;      // where the ring is mapped in the owner, 0 if it isnt
    wait_queue_t wait;
} audio_device_t;

struct task;

// ring_phys has to be periods * period_bytes long, page sized and zeroed
audio_device_t *audio_device_register(const char *name, const audio_driver_t *driver, void *priv,
                                      paddr_t ring_phys, size_t period_bytes, uint32_t periods,
                                      uint32_t rate, uint32_t channels);

// the driver tells us which period the hardware is on now, anything before it is zeroed and handed back
void audio_device_advance(audio_device_t *dev, uint32_t hw_period);

// stops the card if task was the one holding it
void audio_device_task_exit(struct task *task);
