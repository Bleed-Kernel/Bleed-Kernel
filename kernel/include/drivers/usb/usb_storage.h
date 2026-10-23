#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/spinlock.h>
#include <drivers/usb/usb.h>

#define USB_CLASS_MASS_STORAGE  0x08
#define USB_MSC_SUBCLASS_SCSI   0x06
#define USB_MSC_PROTO_BOT       0x50
#define USB_MSC_PROTO_UAS       0x62    // usb attached scsi, usually an alt setting next to BOT

#define USB_MSC_REQ_GET_MAX_LUN 0xFE
#define USB_MSC_REQ_RESET       0xFF

#define USB_STORAGE_IOC_INFO    0x5501  // struct usb_storage_info out
#define USB_STORAGE_IOC_EJECT   0x5502  // unmount, stop it and take its /dev nodes away

struct usb_storage_info {
    char     vendor[9];
    char     product[17];
    char     revision[5];
    char     _pad[1];
    char     mount[64];

    uint64_t sectors;
    uint32_t sector_size;
    uint32_t _pad2;
};

#define USB_STORAGE_MAX         4
#define USB_STORAGE_MAX_NODES   16      // /dev/usbN plus its partitions
#define USB_STORAGE_BOUNCE_PAGES 16
#define USB_STORAGE_BOUNCE_SIZE (USB_STORAGE_BOUNCE_PAGES * PAGE_SIZE)

typedef struct INode INode_t;

typedef struct usb_storage {
    bool          used;
    int           index;
    uint32_t      gen;
    usb_device_t *dev;
    uint8_t       interface;
    uint8_t       ep_in;
    uint8_t       ep_out;
    uint8_t       lun;
    uint32_t      tag;              // goes up every command so a stale CSW cant fool us

    uint8_t      *bounce;           // data stage, the controller dmas in and out of this
    paddr_t       bounce_phys;
    uint8_t      *cmd;              // CBW and CSW live here
    paddr_t       cmd_phys;

    uint64_t      sector_count;
    uint32_t      sector_size;
    bool          cdb16;            // too big for the 10 byte commands
    char          vendor[9];
    char          product[17];
    char          revision[5];

    INode_t      *nodes[USB_STORAGE_MAX_NODES];
    char          names[USB_STORAGE_MAX_NODES][16];
    int           node_count;
    bool          mount_tried;      // automount gets one go per plug
    bool          ejected;          // still plugged in but done with, waits to be pulled

    spinlock_t    lock;
} usb_storage_t;

/// @brief Look through a config descriptor for a bulk only SCSI interface and bring it up as /dev/usbN
/// @param dev configured device
/// @param config full config descriptor
/// @param len length of config
/// @return true if a storage interface was claimed
bool usb_storage_probe(usb_device_t *dev, const uint8_t *config, size_t len);

// Unmount and unregister every drive on dev
void usb_storage_disconnect(usb_device_t *dev);

// Mount any drive that isnt yet, /mnt/usb for the first one and /mnt/usbN after that
void usb_storage_automount(void);
