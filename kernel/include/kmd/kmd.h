#pragma once

// what a kernel mode driver (.KMD) sees

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pci/pci.h>
#include <cpu/irq.h>

#define KMD_ABI             1           // bump when anything in here changes shape
#define KMD_MAGIC           0x444D4B42  // "BKMD"
#define KMD_ANY             0xFFFFFFFFu
#define KMD_NAME_MAX        32
#define KMD_DIR             "/initrd/drivers"
#define KMD_CONF            "/initrd/etc/drivers.conf"

typedef enum {
    KMD_BUS_PCI = 1,
} kmd_bus_t;

// a pci device the driver wants, KMD_ANY in a field matches anything
typedef struct kmd_pci_match {
    uint32_t vendor;
    uint32_t device;
    uint32_t class_code;
    uint32_t subclass;
    uint32_t prog_if;
} kmd_pci_match_t;

#define KMD_PCI_ID(v, d)        { (v), (d), KMD_ANY, KMD_ANY, KMD_ANY }
#define KMD_PCI_CLASS(c, s, p)  { KMD_ANY, KMD_ANY, (c), (s), (p) }
#define KMD_PCI_END             { 0, 0, 0, 0, 0 }

struct kmd;

typedef struct kmd_device {
    kmd_bus_t     bus;
    pci_device_t  pci;
    void         *priv;
    struct kmd   *owner;
} kmd_device_t;

// what the kernel calls on a driver and in this order
typedef struct kmd_driver {
    const char            *description;
    const kmd_pci_match_t *pci;

    int  (*load)(void); 
    int  (*probe)(kmd_device_t *dev);   // every device in .pci nothing else has, 0 keeps it
    void (*start)(void);                // once the scheduler is running, for kernel tasks
    int  (*remove)(kmd_device_t *dev);  // give the device back, nonzero means its busy
    int  (*unload)(void);               // last call before the code is freed, without it the driver stays
} kmd_driver_t;

#define KMD_DRIVER(id)                                                                          \
    __attribute__((section(".kmd_abi"), used)) static const uint32_t id##_abi[2] =              \
        { KMD_MAGIC, KMD_ABI };                                                                 \
    __attribute__((section(".kmd"), used)) const kmd_driver_t id

// sdk/kmd.mk writes this from KMD_DEPENDS in the makefile so drivers dont need to
#define KMD_DEPENDS(list) \
    __attribute__((section(".kmd_depends"), used)) static const char kmd_depends_list[] = list

// every pci device the kernel knows of, bound or not
size_t kmd_device_count(void);
kmd_device_t *kmd_device_get(size_t index);
