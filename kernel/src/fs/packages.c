/*
    packages bpm installs live on a drive (<drive>/opt/<name>) but the root filesystem is the
    ramdisk, so the links that put their programs in $PATH are made again here each time a
    drive turns up. bpm makes them itself when it installs something
*/

#include <fs/packages.h>
#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <user/user_mount.h>
#include <user/errno.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

#define INFO_MAX    4096
#define LINK_PATH   512

static void mkdir_p(const char *path) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; ; p++) {
        if (*p != '/' && *p)
            continue;
        char c = *p;
        *p = '\0';
        vfs_mkdir(buf);
        *p = c;
        if (!c)
            break;
    }
}

// the exec= line of a PKGINFO, empty if theres none
static void read_exec(const char *info_path, char *out, size_t size) {
    out[0] = '\0';
    path_t   p     = vfs_path_from_abs(info_path);
    INode_t *inode = NULL;
    if (vfs_lookup(&p, &inode) < 0)
        return;

    char *text = kmalloc(INFO_MAX + 1);
    long  n    = text ? inode_read(inode, text, INFO_MAX, 0) : -1;
    vfs_drop(inode);
    if (n <= 0) {
        kfree(text);
        return;
    }
    text[n] = '\0';

    for (char *line = text; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (!strncmp(line, "exec=", 5)) {
            snprintf(out, size, "%s", line + 5);
            break;
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(text);
}

// one link for each program name in exec, a link that already exists is left alone
static void link_programs(const char *dir, char *exec) {
    char link[LINK_PATH], target[LINK_PATH];
    char *p = exec;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        char *name = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r')
            p++;
        if (p == name)
            break;
        char c = *p;
        *p = '\0';

        if (!strchr(name, '/')) {
            snprintf(link, sizeof(link), PACKAGES_LINKS "/%s", name);
            snprintf(target, sizeof(target), "%s/%s", dir, name);
            int r = vfs_symlink_at(target, AT_FDCWD, link);
            if (r == 0)
                serial_printf(LOG_INFO "packages: %s -> %s\n", link, target);
        }
        *p = c;
    }
}

static void scan_drive(const char *mount) {
    char pkgs[LINK_PATH];
    snprintf(pkgs, sizeof(pkgs), "%s/" PACKAGES_DIR, mount);
    path_t   p   = vfs_path_from_abs(pkgs);
    INode_t *dir = NULL;
    if (vfs_lookup(&p, &dir) < 0)
        return;
    if (dir->type != INODE_DIRECTORY) {
        vfs_drop(dir);
        return;
    }

    char *pkg  = kmalloc(LINK_PATH);
    char *info = kmalloc(LINK_PATH);
    char *exec = kmalloc(256);
    for (size_t i = 0; pkg && info && exec; i++) {
        INode_t *child = NULL;
        if (vfs_readdir(dir, i, &child) < 0 || !child)
            break;
        // .<name>.part and .old are bpm mid install
        int skip = child->name[0] == '.' || child->type != INODE_DIRECTORY;
        snprintf(pkg, LINK_PATH, "%s/%s", pkgs, child->name);
        vfs_drop(child);
        if (skip)
            continue;

        snprintf(info, LINK_PATH, "%s/.bpkg/PKGINFO", pkg);
        read_exec(info, exec, 256);
        if (exec[0])
            link_programs(pkg, exec);
    }
    kfree(pkg);
    kfree(info);
    kfree(exec);
    vfs_drop(dir);
}

// links whose target went with a drive
static void prune_links(void) {
    path_t   p   = vfs_path_from_abs(PACKAGES_LINKS);
    INode_t *dir = NULL;
    if (vfs_lookup(&p, &dir) < 0)
        return;

    char *path = kmalloc(LINK_PATH);
    for (size_t i = 0; path; ) {
        INode_t *child = NULL;
        if (vfs_readdir(dir, i, &child) < 0 || !child)
            break;
        int is_link = child->type == INODE_SYMLINK;
        snprintf(path, LINK_PATH, PACKAGES_LINKS "/%s", child->name);
        vfs_drop(child);

        // readdir is index based, an unlink moves the next one into this slot
        path_t   lp     = vfs_path_from_abs(path);
        INode_t *target = NULL;
        if (is_link && vfs_lookup(&lp, &target) < 0 && vfs_unlink(path) == 0) {
            serial_printf(LOG_INFO "packages: %s pointed at something thats gone\n", path);
            continue;
        }
        if (target)
            vfs_drop(target);
        i++;
    }
    kfree(path);
    vfs_drop(dir);
}

void packages_scan(void) {
    unsigned long flags = irq_push();
    mkdir_p(PACKAGES_LINKS);
    prune_links();

    user_mount_info_t info;
    for (int i = 1; vfs_mount_info(i, &info) == 0; i++) {
        if (!strncmp(info.path, "/mnt/", 5))
            scan_drive(info.path);
    }
    irq_restore(flags);
}
