#include <drivers/usb/usb_storage.h>
#include <drivers/usb/usb_bot.h>
#include <drivers/usb/usb_scsi.h>
#include <drivers/usb/usb_hcd.h>
#include <drivers/usb/usb.h>
#include <drivers/serial/serial.h>
#include <devices/devices.h>
#include <fs/vfs.h>
#include <fs/partition.h>
#include <fs/vfs_mount.h>
#include <fs/data_image.h>
#include <mm/kalloc.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <sched/scheduler.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static usb_storage_t drives[USB_STORAGE_MAX];
static uint32_t      next_gen = 1;

// a whole disk or one partition on it, both show up under /dev.
typedef struct usb_storage_blk {
    usb_storage_t *ms;
    uint32_t       gen;
    uint64_t       lba_start;
    uint64_t       sector_count;
} usb_storage_blk_t;

static bool usb_storage_alive(const usb_storage_blk_t *blk) {
    usb_storage_t *ms = blk->ms;
    return ms->used && ms->gen == blk->gen && !ms->ejected && ms->dev && !ms->dev->gone;
}

// the window holding lba, read in whole if it isnt there already
static usb_storage_window_t *usb_storage_window(usb_storage_t *ms, uint64_t lba) {
    uint32_t per   = USB_STORAGE_BOUNCE_SIZE / ms->sector_size;
    uint64_t start = lba - lba % per;

    usb_storage_window_t *pick = &ms->cache[0];
    for (int i = 0; i < USB_STORAGE_CACHE_WINDOWS; i++) {
        usb_storage_window_t *w = &ms->cache[i];
        if (w->sectors && w->lba == start) {
            w->last_used = ++ms->cache_clock;
            return w;
        }
        // empty ones first, then whoever was used longest ago
        if (!w->sectors) {
            if (pick->sectors)
                pick = w;
        } else if (pick->sectors && w->last_used < pick->last_used) {
            pick = w;
        }
    }

    if (!pick->data && !(pick->data = kmalloc(USB_STORAGE_BOUNCE_SIZE)))
        return NULL;

    uint32_t sectors = per;
    if (start + sectors > ms->sector_count)
        sectors = (uint32_t)(ms->sector_count - start);

    pick->sectors = 0;
    if (usb_scsi_rw(ms, start, sectors, false) < 0)
        return NULL;
    memcpy(pick->data, ms->bounce, (size_t)sectors * ms->sector_size);
    pick->lba       = start;
    pick->sectors   = sectors;
    pick->last_used = ++ms->cache_clock;
    return pick;
}

// a write makes whatever the windows had of those sectors stale
static void usb_storage_window_drop(usb_storage_t *ms, uint64_t lba, uint32_t sectors) {
    for (int i = 0; i < USB_STORAGE_CACHE_WINDOWS; i++) {
        usb_storage_window_t *w = &ms->cache[i];
        if (w->sectors && lba < w->lba + w->sectors && w->lba < lba + sectors)
            w->sectors = 0;
    }
}

// reads come out of the windows, writes go through the bounce buffer a chunk at a time
static long usb_storage_io(usb_storage_blk_t *blk, uint8_t *buf, size_t count,
                           size_t offset, bool write) {
    usb_storage_t *ms = blk->ms;

    spinlock_acquire(&ms->lock);
    if (!usb_storage_alive(blk)) {
        spinlock_release(&ms->lock);
        return -1;
    }

    uint32_t ss   = ms->sector_size;
    uint64_t size = blk->sector_count * ss;
    if (offset >= size) {
        spinlock_release(&ms->lock);
        return 0;
    }
    if (count > size - offset)
        count = size - offset;

    size_t done = 0;
    while (done < count) {
        uint64_t pos     = offset + done;
        uint64_t lba     = blk->lba_start + pos / ss;
        size_t   skip    = pos % ss;
        size_t   take    = count - done;

        if (!write) {
            usb_storage_window_t *w = usb_storage_window(ms, lba);
            if (!w)
                break;
            size_t at = (size_t)(lba - w->lba) * ss + skip;
            if (take > (size_t)w->sectors * ss - at)
                take = (size_t)w->sectors * ss - at;
            memcpy(buf + done, w->data + at, take);
            done += take;
            continue;
        }

        if (take > USB_STORAGE_BOUNCE_SIZE - skip)
            take = USB_STORAGE_BOUNCE_SIZE - skip;
        uint32_t sectors = (uint32_t)((skip + take + ss - 1) / ss);
        bool     partial = skip || (skip + take) % ss;

        usb_storage_window_drop(ms, lba, sectors);
        if (partial) {
            if (usb_scsi_rw(ms, lba, sectors, false) < 0)
                break;
        }
        memcpy(ms->bounce + skip, buf + done, take);
        if (usb_scsi_rw(ms, lba, sectors, true) < 0)
            break;
        done += take;
    }

    spinlock_release(&ms->lock);
    return done ? (long)done : -1;
}

static long usb_storage_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    usb_storage_blk_t *blk = inode->internal_data;
    if (!blk || !buf || count == 0) return 0;
    return usb_storage_io(blk, buf, count, offset, false);
}

static long usb_storage_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    usb_storage_blk_t *blk = inode->internal_data;
    if (!blk || !buf || count == 0) return 0;
    return usb_storage_io(blk, (uint8_t *)buf, count, offset, true);
}

static size_t usb_storage_size(INode_t *inode) {
    usb_storage_blk_t *blk = inode->internal_data;
    if (!blk || blk->gen != blk->ms->gen || !blk->ms->used)
        return 0;
    return (size_t)(blk->sector_count * blk->ms->sector_size);
}

static int usb_storage_info(usb_storage_t *ms, task_t *task, void *arg) {
    struct usb_storage_info info;
    memset(&info, 0, sizeof(info));
    memcpy(info.vendor,   ms->vendor,   sizeof(info.vendor));
    memcpy(info.product,  ms->product,  sizeof(info.product));
    memcpy(info.revision, ms->revision, sizeof(info.revision));
    info.sectors     = ms->sector_count;
    info.sector_size = ms->sector_size;

    for (int n = 0; n < ms->node_count; n++) {
        if (vfs_device_mount_path(ms->nodes[n], info.mount, sizeof(info.mount)) == 0)
            break;
    }
    return copy_to_user(task, arg, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
}

// graceful eject
static int usb_storage_eject(usb_storage_t *ms) {
    for (int n = 0; n < ms->node_count; n++) {
        if (vfs_device_busy(ms->nodes[n])) {
            serial_printf(LOG_WARN "usbstorage%d: wont eject, /dev/%s has files open on it\n",
                          ms->index, ms->names[n]);
            return -EBUSY;
        }
    }

    for (int n = 0; n < ms->node_count; n++)
        vfs_umount_device(ms->nodes[n]);

    spinlock_acquire(&ms->lock);
    usb_scsi_eject(ms);
    ms->ejected = true;
    spinlock_release(&ms->lock);

    for (int n = 0; n < ms->node_count; n++)
        device_unregister(ms->names[n]);
    ms->node_count = 0;

    serial_printf(LOG_OK "usbstorage%d: %s %s ejected, safe to unplug\n", ms->index, ms->vendor, ms->product);
    return 0;
}

static int usb_storage_ioctl(INode_t *inode, unsigned long request, void *arg) {
    usb_storage_blk_t *blk  = inode->internal_data;
    task_t            *task = get_current_task();
    if (!blk || !task)
        return -ENODEV;

    usb_storage_t *ms = blk->ms;
    if (!ms->used || ms->gen != blk->gen || ms->ejected || !ms->dev || ms->dev->gone)
        return -ENODEV;

    switch (request) {
        case USB_STORAGE_IOC_INFO:
            return arg ? usb_storage_info(ms, task, arg) : -EFAULT;
        case USB_STORAGE_IOC_EJECT:
            return usb_storage_eject(ms);
    }
    return -ENOTTY;
}

static INodeOps_t usb_storage_ops = {
    .read  = usb_storage_read,
    .write = usb_storage_write,
    .size  = usb_storage_size,
    .ioctl = usb_storage_ioctl,
};

static bool usb_storage_register(usb_storage_t *ms, const char *name,
                                 uint64_t lba_start, uint64_t sector_count) {
    if (ms->node_count >= USB_STORAGE_MAX_NODES) {
        serial_printf(LOG_WARN "usbstorage%d: no room for /dev/%s, only %u nodes per drive\n",
                      ms->index, name, USB_STORAGE_MAX_NODES);
        return false;
    }

    usb_storage_blk_t *blk = kmalloc(sizeof(*blk));
    if (!blk) return false;
    memset(blk, 0, sizeof(*blk));
    blk->ms           = ms;
    blk->gen          = ms->gen;
    blk->lba_start    = lba_start;
    blk->sector_count = sector_count;

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(blk); return false; }
    memset(inode, 0, sizeof(*inode));
    inode->type          = INODE_DEVICE;
    inode->ops           = &usb_storage_ops;
    inode->internal_data = blk;
    inode->shared        = 1;

    if (device_register(inode, (char *)name) < 0) {
        serial_printf(LOG_WARN "usbstorage%d: failed to register /dev/%s\n", ms->index, name);
        kfree(inode);
        kfree(blk);
        return false;
    }

    ms->nodes[ms->node_count] = inode;
    strncpy(ms->names[ms->node_count], name, sizeof(ms->names[0]) - 1);
    ms->node_count++;
    return true;
}

static void usb_storage_register_part(void *drive_obj, int drive_idx, int part_idx,
                                      uint64_t start, uint64_t count, uint8_t type) {
    usb_storage_t *ms = drive_obj;
    char name[16];
    snprintf(name, sizeof(name), "usb%dp%d", drive_idx, part_idx + 1);

    if (usb_storage_register(ms, name, start, count))
        serial_printf(LOG_OK "usbstorage%d: registered /dev/%s (lba=%llu sectors=%llu type=0x%02x)\n",
                      ms->index, name, start, count, type);
}

// partition_probe reads into a 512 byte buffer of its own
static int usb_storage_read_sectors(void *drive_obj, uint64_t lba, uint16_t count, void *buf) {
    usb_storage_t *ms = drive_obj;

    spinlock_acquire(&ms->lock);
    int r = ms->dev->gone ? USB_ERR_GONE : usb_scsi_rw(ms, lba, count, false);
    if (r == 0)
        memcpy(buf, ms->bounce, (size_t)count * ms->sector_size);
    spinlock_release(&ms->lock);
    return r;
}

static bool usb_storage_find_endpoints(usb_storage_t *ms, const uint8_t *config, size_t len,
                                       size_t start, const usb_endpoint_desc_t **in,
                                       const usb_endpoint_desc_t **out,
                                       const usb_ss_companion_desc_t **in_ss,
                                       const usb_ss_companion_desc_t **out_ss) {
    const usb_ss_companion_desc_t **last_ss = NULL;
    *in = *out = NULL;
    *in_ss = *out_ss = NULL;

    for (size_t off = start; off + 2 <= len; ) {
        uint8_t desc_len  = config[off];
        uint8_t desc_type = config[off + 1];
        if (desc_len < 2 || off + desc_len > len) break;
        if (desc_type == USB_DESC_INTERFACE) break;

        if (desc_type == USB_DESC_ENDPOINT && desc_len >= sizeof(usb_endpoint_desc_t)) {
            const usb_endpoint_desc_t *ep = (const usb_endpoint_desc_t *)&config[off];
            last_ss = NULL;

            if ((ep->attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_BULK) {
                if ((ep->address & USB_ENDPOINT_IN) && !*in) {
                    *in     = ep;
                    last_ss = in_ss;
                } else if (!(ep->address & USB_ENDPOINT_IN) && !*out) {
                    *out    = ep;
                    last_ss = out_ss;
                }
            }
        } else if (desc_type == USB_DESC_SS_COMPANION && last_ss &&
                   desc_len >= sizeof(usb_ss_companion_desc_t)) {
            *last_ss = (const usb_ss_companion_desc_t *)&config[off];
        }

        off += desc_len;
    }

    if (!*in || !*out) {
        serial_printf(LOG_WARN "usbstorage%d: interface is missing a bulk %s endpoint\n",
                      ms->index, *in ? "out" : "in");
        return false;
    }
    return true;
}

static bool usb_storage_set_interface(usb_device_t *dev, uint8_t interface, uint8_t alt) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RCPT_INTERFACE,
        .request      = USB_REQ_SET_INTERFACE,
        .value        = alt,
        .index        = interface,
    };
    return usb_control(dev, &setup, NULL) >= 0;
}

static bool usb_storage_start(usb_storage_t *ms, const uint8_t *config, size_t len,
                              size_t iface_off) {
    const usb_interface_desc_t *iface = (const usb_interface_desc_t *)&config[iface_off];
    const usb_endpoint_desc_t *in, *out;
    const usb_ss_companion_desc_t *in_ss, *out_ss;

    if (!usb_storage_find_endpoints(ms, config, len, iface_off + iface->length,
                                    &in, &out, &in_ss, &out_ss))
        return false;

    ms->interface = iface->interface_number;
    ms->ep_in     = in->address;
    ms->ep_out    = out->address;

    serial_printf(LOG_INFO "usbstorage%d: interface %u alt %u, bulk in 0x%02x (%u), bulk out 0x%02x (%u)\n",
                  ms->index, ms->interface, iface->alt_setting, ms->ep_in,
                  in->max_packet & USB_ENDPOINT_MPS_MASK, ms->ep_out,
                  out->max_packet & USB_ENDPOINT_MPS_MASK);

    if (iface->alt_setting && !usb_storage_set_interface(ms->dev, ms->interface, iface->alt_setting)) {
        serial_printf(LOG_WARN "usbstorage%d: SET_INTERFACE to alt %u failed\n",
                      ms->index, iface->alt_setting);
        return false;
    }

    if (usb_bulk_open(ms->dev, in, in_ss) < 0 || usb_bulk_open(ms->dev, out, out_ss) < 0) {
        serial_printf(LOG_WARN "usbstorage%d: couldnt open the bulk endpoints\n", ms->index);
        return false;
    }

    ms->bounce = usb_dma_alloc(USB_STORAGE_BOUNCE_PAGES, &ms->bounce_phys, true);
    ms->cmd    = usb_dma_alloc(1, &ms->cmd_phys, true);
    if (!ms->bounce || !ms->cmd) {
        serial_printf(LOG_ERROR "usbstorage%d: OOM allocating dma buffers\n", ms->index);
        return false;
    }

    uint8_t max_lun = usb_bot_max_lun(ms);
    serial_printf(LOG_INFO "usbstorage%d: %u lun%s, only using lun 0\n",
                  ms->index, max_lun + 1, max_lun ? "s" : "");
    ms->lun = 0;

    return usb_scsi_inquiry(ms) && usb_scsi_wait_ready(ms) && usb_scsi_read_capacity(ms);
}

// caller holds the lock or nobody else can see ms yet
static void usb_storage_free(usb_storage_t *ms) {
    for (int i = 0; i < USB_STORAGE_CACHE_WINDOWS; i++) {
        kfree(ms->cache[i].data);
        ms->cache[i].data    = NULL;
        ms->cache[i].sectors = 0;
    }
    if (ms->bounce) pmm_free_pages(ms->bounce_phys, USB_STORAGE_BOUNCE_PAGES);
    if (ms->cmd)    pmm_free_pages(ms->cmd_phys, 1);
    ms->bounce = NULL;
    ms->cmd    = NULL;
    ms->dev    = NULL;
    ms->used   = false;
}

// pick the bulk only interface, first wins
static size_t usb_storage_find_interface(const uint8_t *config, size_t len, bool *found_other) {
    *found_other = false;

    for (size_t off = 0; off + 2 <= len; ) {
        uint8_t desc_len  = config[off];
        uint8_t desc_type = config[off + 1];
        if (desc_len < 2 || off + desc_len > len) break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= sizeof(usb_interface_desc_t)) {
            const usb_interface_desc_t *iface = (const usb_interface_desc_t *)&config[off];
            if (iface->class_code == USB_CLASS_MASS_STORAGE) {
                if (iface->subclass == USB_MSC_SUBCLASS_SCSI && iface->protocol == USB_MSC_PROTO_BOT)
                    return off;

                serial_printf(LOG_INFO "usbstorage: skipping mass storage alt %u - subclass 0x%02x protocol 0x%02x%s\n",
                              iface->alt_setting, iface->subclass, iface->protocol,
                              iface->protocol == USB_MSC_PROTO_UAS ? " (UAS)" : "");
                *found_other = true;
            }
        }
        off += desc_len;
    }
    return 0;
}

bool usb_storage_probe(usb_device_t *dev, const uint8_t *config, size_t len) {
    bool   found_other = false;
    size_t iface_off   = usb_storage_find_interface(config, len, &found_other);
    if (!iface_off) {
        if (found_other)
            serial_printf(LOG_WARN "usbstorage: %04x:%04x is mass storage but not SCSI over bulk only, we dont speak that\n",
                          dev->vendor_id, dev->product_id);
        return false;
    }

    if (!dev->ops->bulk) {
        serial_printf(LOG_WARN "usbstorage: %04x:%04x is on %s which cant do bulk transfers yet\n",
                      dev->vendor_id, dev->product_id, dev->ops->name);
        return false;
    }
    // lowest free slot
    usb_storage_t *ms = NULL;
    for (int i = 0; i < USB_STORAGE_MAX; i++) {
        if (!drives[i].used) {
            ms = &drives[i];
            break;
        }
    }
    if (!ms) {
        serial_printf(LOG_WARN "usbstorage: too many drives\n");
        return false;
    }

    int index = (int)(ms - drives);
    memset(ms, 0, sizeof(*ms));
    ms->used  = true;
    ms->index = index;
    ms->gen   = next_gen++;
    ms->dev   = dev;
    spinlock_init(&ms->lock);

    serial_printf(LOG_INFO "usbstorage%d: bringing up %04x:%04x\n", ms->index,
                  dev->vendor_id, dev->product_id);

    if (!usb_storage_start(ms, config, len, iface_off)) {
        serial_printf(LOG_WARN "usbstorage%d: bring up failed, leaving it alone\n", ms->index);
        usb_storage_free(ms);
        return false;
    }

    char name[16];
    snprintf(name, sizeof(name), "usb%d", ms->index);
    if (!usb_storage_register(ms, name, 0, ms->sector_count)) {
        usb_storage_free(ms);
        return false;
    }

    serial_printf(LOG_OK "usbstorage%d: /dev/%s - %s %s %s (%llu sectors, %u bytes/sector)\n",
                  ms->index, name, ms->vendor, ms->product, ms->revision,
                  ms->sector_count, ms->sector_size);

    if (ms->sector_size == 512)
        partition_probe(ms, ms->index, usb_storage_read_sectors, usb_storage_register_part);
    else
        serial_printf(LOG_WARN "usbstorage%d: partition tables on %u byte sectors arent supported, use /dev/%s directly\n",
                      ms->index, ms->sector_size, name);
    return true;
}

void usb_storage_disconnect(usb_device_t *dev) {
    for (int i = 0; i < USB_STORAGE_MAX; i++) {
        usb_storage_t *ms = &drives[i];
        if (!ms->used || ms->dev != dev) continue;

        if (ms->ejected)
            serial_printf(LOG_INFO "usbstorage%d: %s %s unplugged after its eject\n",
                          ms->index, ms->vendor, ms->product);
        else
            serial_printf(LOG_WARN "usbstorage%d: %s %s pulled out, unmounting without a sync\n",
                          ms->index, ms->vendor, ms->product);

        // the mounts first, a filesystem letting go might still try to write and that
        // has to fail on its own without us holding the lock
        for (int n = 0; n < ms->node_count; n++)
            vfs_umount_device(ms->nodes[n]);
        for (int n = 0; n < ms->node_count; n++) {
            device_unregister(ms->names[n]);
            serial_printf(LOG_INFO "usbstorage%d: /dev/%s removed\n", ms->index, ms->names[n]);
        }

        // whoever was mid command sees gone and bails, once we have the lock nobody is using it
        spinlock_acquire(&ms->lock);
        usb_storage_free(ms);
        spinlock_release(&ms->lock);
    }
}

void usb_storage_automount(void) {
    bool mounted = false;
    for (int i = 0; i < USB_STORAGE_MAX; i++) {
        usb_storage_t *ms = &drives[i];
        if (!ms->used || ms->mount_tried || ms->ejected || ms->dev->gone) continue;
        ms->mount_tried = true;

        char part[16];
        snprintf(part, sizeof(part), "usb%dp1", ms->index);
        const char *name = device_get_by_name(part) ? part : ms->names[0];

        char path[16];
        if (ms->index == 0)
            snprintf(path, sizeof(path), "/mnt/usb");
        else
            snprintf(path, sizeof(path), "/mnt/usb%d", ms->index);

        int r = vfs_mount(path, device_get_by_name(name));
        mounted |= r == 0;
        if (r == 0)
            serial_printf(LOG_OK "usbstorage%d: /dev/%s mounted at %s\n", ms->index, name, path);
        else
            serial_printf(LOG_WARN "usbstorage%d: couldnt mount /dev/%s at %s (%d)\n",
                          ms->index, name, path, r);
    }

    // a stick with data.img on it next to the iso. this runs every 100ms 
    if (mounted)
        data_image_scan();
}
