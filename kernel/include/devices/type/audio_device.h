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
#define AUDIO_IOC_GET_OWNER  0x510A  // pid of a program playing, 0 for none
#define AUDIO_IOC_WAIT       0x510B  // sleep till the hardware finishes the period its on
#define AUDIO_IOC_GET_VOLUME 0x510C  // struct audio_volume out, what SET_VOLUME last set
#define AUDIO_IOC_GET_OUTPUTS 0x510D // struct audio_outputs out, jacks are read when you ask
#define AUDIO_IOC_SET_OUTPUT  0x510E // int32_t in, an index from GET_OUTPUTS or AUDIO_OUTPUT_AUTO

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

#define AUDIO_MAX_OUTPUTS     8
#define AUDIO_OUTPUT_NAME     28
#define AUDIO_OUTPUT_AUTO     (-1)      // the card picks from what is plugged in

#define AUDIO_OUT_SPEAKER     1
#define AUDIO_OUT_HEADPHONES  2
#define AUDIO_OUT_LINE        3

#define AUDIO_OUTPUT_ACTIVE   (1u << 0) // sound is going here right now
#define AUDIO_OUTPUT_DETECT   (1u << 1) // the card can tell if something is plugged in
#define AUDIO_OUTPUT_PLUGGED  (1u << 2)

struct audio_output {
    char    name[AUDIO_OUTPUT_NAME];
    uint8_t type;       // AUDIO_OUT_*
    uint8_t flags;      // AUDIO_OUTPUT_*
    uint8_t _pad[2];
};

struct audio_outputs {
    uint32_t count;
    int32_t  selected;  // an index, or AUDIO_OUTPUT_AUTO
    struct audio_output out[AUDIO_MAX_OUTPUTS];
};

// what a sound card driver gives the audio device
typedef struct audio_driver {
    const char *name;
    int      (*start)(void *priv);
    void     (*stop)(void *priv);
    uint32_t (*hw_offset)(void *priv);
    int      (*set_rate)(void *priv, uint32_t rate);
    int      (*set_volume)(void *priv, const struct audio_volume *vol); 
    int      (*get_outputs)(void *priv, struct audio_outputs *outputs);
    int      (*set_output)(void *priv, int32_t index);
} audio_driver_t;

#define AUDIO_STREAMS       8
#define AUDIO_JOIN_AHEAD    2

// one programs audio. its ring is where it writes
typedef struct audio_stream {
    bool        used;
    bool        running;
    uint64_t    pid;
    void       *file;
    uint8_t     prio;
    paddr_t     ring_phys;
    uint8_t    *ring;
    uintptr_t   map;

    // write_pos is bytes since this stream started, origin is where that start is on the cards 
    uint64_t    origin;
    uint64_t    write_pos;
    uint64_t    underruns;
} audio_stream_t;

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
    uint64_t    played;
    uint64_t    heard;

    audio_stream_t streams[AUDIO_STREAMS];

    struct audio_volume volume;
    wait_queue_t wait;
} audio_device_t;

struct task;

// ring_phys has to be periods * period_bytes long, page sized and zeroed
audio_device_t *audio_device_register(const char *name, const audio_driver_t *driver, void *priv,
                                      paddr_t ring_phys, size_t period_bytes, uint32_t periods,
                                      uint32_t rate, uint32_t channels);

int audio_device_unregister(audio_device_t *dev);

// the driver tells us which period the hardware is on now, whats behind it is zeroed for the next lap
void audio_device_advance(audio_device_t *dev, uint32_t hw_period);

// the tasks stream goes, what it already wrote still plays
void audio_device_task_exit(struct task *task);
