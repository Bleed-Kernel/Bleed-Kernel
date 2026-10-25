export MAKEFLAGS = -j8

QEMU_NAME ?= The Bleed Kernel

IMAGE_NAME := bleed-kernel
OBJDIR := bin/obj
KERNEL_BIN := bin/bleed-kernel
OVMF_FW := edk2-ovmf/OVMF-pure-efi.fd
MEMSZ := 1G
PROC_VERSION_FILE := initrd/proc/version
BUILD_GIT_HASH := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
BUILD_GIT_COUNT := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
BUILD_GIT_DIRTY := $(shell if git diff --quiet --ignore-submodules HEAD >/dev/null 2>&1; then echo ""; else echo "-dirty"; fi)
BUILD_VERSION := r$(BUILD_GIT_COUNT)-g$(BUILD_GIT_HASH)$(BUILD_GIT_DIRTY)

MAKEFLAGS += --no-print-directory

IDE_DISK := idedisk.img
SATA_DISK := satadisk.img
NVME_DISK := nvmedisk.img
USB_DISK := usbdisk.img
DISKS := $(IDE_DISK) $(SATA_DISK) $(NVME_DISK) $(USB_DISK)
DISK_SIZE_MB := 128

# programs that put files on the nvme disk with a `disk` make target, found next to us the same
# way local program checkouts are. half life is the only one, its game files live in its own repo
DISK_PROGRAMS = Bleed-HalfLife
DISK_PROGRAM_DIRS = $(foreach p,$(DISK_PROGRAMS),$(wildcard $(USERPROG_SRC)/$(p)))
NVME_FILES := .nvme_files
NVME_DISK_SIZE_MB = $(if $(DISK_PROGRAM_DIRS),1024,$(DISK_SIZE_MB))

LIMINE_VERSION := 12.9.2
LIMINE_URL := https://github.com/Limine-Bootloader/Limine/releases/download/v$(LIMINE_VERSION)/limine-binary.tar.gz
LIMINE_SHA256 := 047e7eeae7de4de62fca4fc3740b621aa37a2038b208f4d0ac993750a18eed86

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S)$(UNAME_M),Darwinarm64)
CC := x86_64-elf-gcc
LD := x86_64-elf-ld
NM := x86_64-elf-nm
AR := x86_64-elf-ar
AS := x86_64-elf-as
USERPROG_TOOLCHAIN := CC=x86_64-elf-gcc AR=x86_64-elf-ar AS=x86_64-elf-as
else
CC := cc
LD := ld
NM := nm
USERPROG_TOOLCHAIN :=
endif

CFLAGS := -g -O2 -Wall -Werror -Wextra -std=gnu11 \
          -nostdinc -ffreestanding -fno-stack-protector \
          -fno-stack-check -fno-lto -fno-PIC -fno-pie \
          -ffunction-sections -fdata-sections -fno-omit-frame-pointer \
          -m64 -march=x86-64 -mabi=sysv -mcmodel=kernel \
          -mno-red-zone \
          -msoft-float \
          -mno-80387 \
          -mno-mmx \
          -mno-sse \
          -mno-sse2 \
          -mno-sse3 \
          -mno-ssse3 \
          -mno-sse4 \
          -mno-sse4.1 \
          -mno-sse4.2 \
          -mno-avx \
          -mno-avx2 \
          -mno-avx512f \
          -I kernel/include -I klibc/include \
          -MMD -MP

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --gc-sections \
           -T kernel.lds

KERNEL_OBJ := $(patsubst %.c, $(OBJDIR)/%.o, $(shell find kernel -name '*.c')) \
              $(patsubst %.S, $(OBJDIR)/%.o, $(shell find kernel -name '*.S'))

KLIBC_OBJ := $(patsubst %.c, $(OBJDIR)/%.o, $(shell find klibc -name '*.c')) \
             $(patsubst %.S, $(OBJDIR)/%.o, $(shell find klibc -name '*.S'))

OBJ := $(KERNEL_OBJ) $(KLIBC_OBJ)
DEPS := $(OBJ:.o=.d)
-include $(DEPS)

MK_SYMTAB := bin/tools/mksymtab
KERNEL_NM := bin/kernel.nm
KERNEL_SYM := initrd/etc/kernel.sym

USER_REPOS := \
    "https://github.com/Bleed-Kernel/Verdict-Shell verdict" \
    "https://github.com/Bleed-Kernel/Bleed-Doom doom" \
	"https://github.com/Bleed-Kernel/Bleed-Quake2 quake2" \
	"https://github.com/Bleed-Kernel/Bleed-SpecSeek specseek" \
    "https://github.com/Bleed-Kernel/Bleed-Taskman taskv" \
    "https://github.com/Bleed-Kernel/Bleed-tvi tvi" \
	"https://github.com/Bleed-Kernel/Bleed-Tetris tetris" \
	"https://github.com/Bleed-Kernel/Crime-Scene crime-scene" \
	"https://github.com/Bleed-Kernel/libuikit uikit-demo" \
	"https://github.com/Bleed-Kernel/Bleed-About about" \
	"https://github.com/Bleed-Kernel/Bleed-Cabinet cabinet" \
	"https://github.com/Bleed-Kernel/Bleed-Exhibit exhibit" \
	"https://github.com/Bleed-Kernel/Bleed-Playback playback" \
    "https://github.com/Bleed-Kernel/Bleed-Coreutils cat" \
    "https://github.com/Bleed-Kernel/Bleed-Coreutils echo" \
    "https://github.com/Bleed-Kernel/Bleed-Coreutils write" \
	"https://github.com/Bleed-Kernel/Bleed-Coreutils ioctl" \
	"https://github.com/Bleed-Kernel/Bleed-TCC tcc" \
	"https://github.com/Bleed-Kernel/Bleed-Fastfetch fastfetch" \
	"https://github.com/Bleed-Kernel/Bleed-HalfLife halflife" \
	"https://github.com/Bleed-Kernel/Bleed-HalfLife hlds" \

USER_BIN_DIR := external/
INITRD_BIN := initrd/bin

# build user programs from local checkouts instead of cloning/pulling GitHub,
# e.g. `make run USERPROG_SRC=..` picks up ../Verdict-Shell, ../Bleed-coreutils and ../blibc.
# repos without a local checkout still come from GitHub

#there is also make runlocal for this which is nice, change this to where you keep ur userprograms
USERPROG_SRC ?= ..

.PHONY: all
all: $(IMAGE_NAME).iso $(DISKS)

$(PROC_VERSION_FILE): FORCE
	@mkdir -p $(dir $@)
	@tmp="$@.tmp"; \
	printf "%s\n" "$(BUILD_VERSION)" > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$@" "$$tmp"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
	fi

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "[AS] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJ)
	@mkdir -p $(dir $@)
	@echo "[LD] $@"
	@$(LD) $(LDFLAGS) $(OBJ) -o $@

$(MK_SYMTAB): tools/mksymtab.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@cc -O2 -Wall -Wextra -std=gnu11 $< -o $@

$(KERNEL_NM): $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	@$(NM) -n $(KERNEL_BIN) > $@

$(KERNEL_SYM): $(KERNEL_NM) $(MK_SYMTAB)
	@mkdir -p $(dir $@)
	@$(MK_SYMTAB) $(KERNEL_NM) $@

limine/limine:
	rm -rf limine
	mkdir -p limine
	curl -fsSL -o limine/limine-binary.tar.gz $(LIMINE_URL)
	echo "$(LIMINE_SHA256)  limine/limine-binary.tar.gz" | shasum -a 256 -c -
	tar -xzf limine/limine-binary.tar.gz -C limine --strip-components=1
	rm -f limine/limine-binary.tar.gz

# whats in initrd/lib and initrd/include isnt kept here, the programs that need it
# (tcc, for its headers and libc) put it there with an `initrd` make target
.PHONY: initrd-files
initrd-files:
	@if [ -d initrd/lib ] && [ -d initrd/include ]; then exit 0; fi; \
	set -e; blibc_arg=""; \
	if [ -n "$(USERPROG_SRC)" ] && [ -d "$(USERPROG_SRC)/blibc" ]; then \
		blibc_arg="BLIBC_DIR=$$(cd "$(USERPROG_SRC)/blibc" && pwd)"; \
	fi; \
	for entry in $(USER_REPOS); do \
		repo=$${entry%% *}; \
		name=$${entry##* }; \
		dir=""; \
		if [ -n "$(USERPROG_SRC)" ]; then \
			dir=$$(find "$(USERPROG_SRC)" -maxdepth 1 -type d -iname "$${repo##*/}" | head -n 1); \
		fi; \
		[ -n "$$dir" ] || dir=$(USER_BIN_DIR)/$$name; \
		if [ ! -d "$$dir" ]; then \
			case "$$repo" in *Bleed-TCC) ;; *) continue;; esac; \
			echo "[USER] Cloning $$name for its initrd files"; \
			git clone "$$repo" "$$dir"; \
		fi; \
		grep -q '^initrd:' "$$dir/Makefile" 2>/dev/null || continue; \
		echo "[USER] initrd/lib or initrd/include is missing, getting $$name's files"; \
		$(MAKE) -s -C "$$dir" initrd INITRD=$(abspath initrd) $(USERPROG_TOOLCHAIN) $$blibc_arg; \
	done

.PHONY: userprogs
userprogs:
	@mkdir -p $(USER_BIN_DIR)
	@mkdir -p $(INITRD_BIN)
	@set -e; for entry in $(USER_REPOS); do \
		repo=$${entry%% *}; \
		name=$${entry##* }; \
		dir=$(USER_BIN_DIR)/$$name; \
		local_dir=""; blibc_arg=""; crime_arg=""; audio_arg=""; \
		if [ -n "$(USERPROG_SRC)" ]; then \
			local_dir=$$(find "$(USERPROG_SRC)" -maxdepth 1 -type d -iname "$${repo##*/}" | head -n 1); \
			if [ -d "$(USERPROG_SRC)/blibc" ]; then \
				blibc_arg="BLIBC_DIR=$$(cd "$(USERPROG_SRC)/blibc" && pwd)"; \
			fi; \
			if [ -d "$(USERPROG_SRC)/Crime-Scene" ]; then \
				crime_arg="CRIME_DIR=$$(cd "$(USERPROG_SRC)/Crime-Scene" && pwd)"; \
			fi; \
			if [ -d "$(USERPROG_SRC)/libaudio" ]; then \
				audio_arg="AUDIO_DIR=$$(cd "$(USERPROG_SRC)/libaudio" && pwd)"; \
			fi; \
		fi; \
		if [ -n "$$local_dir" ]; then \
			echo "[USER] Using local checkout $$local_dir for $$name"; \
			dir=$$local_dir; \
		elif [ ! -d "$$dir" ]; then \
			echo "[USER] Cloning $$name from $$repo"; \
			git clone "$$repo" "$$dir"; \
		else \
			echo "[USER] Pulling latest for $$name"; \
			(cd "$$dir" && git pull --rebase); \
		fi; \
		echo "[USER] Preparing blibc for $$name"; \
		$(MAKE) -s -C "$$dir" blibc $(USERPROG_TOOLCHAIN) $$blibc_arg; \
		echo "[USER] Building $$name"; \
		$(MAKE) -s -C "$$dir" $(USERPROG_TOOLCHAIN) $$blibc_arg $$crime_arg $$audio_arg; \
		if [ -f "$$dir/bin/$$name" ]; then \
			COPYFILE_DISABLE=1 cp "$$dir/bin/$$name" $(INITRD_BIN)/$$name; \
		else \
			echo "ERROR: $$name binary not found"; \
			exit 1; \
		fi; \
		if grep -q '^initrd:' "$$dir/Makefile"; then \
			echo "[USER] Copying $$name's files into the initrd"; \
			$(MAKE) -s -C "$$dir" initrd INITRD=$(abspath initrd) $(USERPROG_TOOLCHAIN) $$blibc_arg; \
		fi; \
	done

define build_initrd_tar
	@mkdir -p initrd
	@find initrd -name '._*' -delete
	@find initrd -name '.DS_Store' -delete
	@# no xattrs, macos puts com.apple.provenance on everything and tar would store it as a pax
	@# header, which the initrd shows as a PaxHeader directory
	@COPYFILE_DISABLE=1 tar --no-xattrs -cf initrd/initrd.tar initrd/*/* initrd/*.*
endef

define build_iso
	@rm -rf iso_root
	@mkdir -p iso_root/boot
	@cp $(KERNEL_BIN) iso_root/boot/
	@cp wallpaper.png iso_root/
	@mkdir -p iso_root/boot/limine
	@cp limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin iso_root/boot/limine/
	@mkdir -p iso_root/EFI/BOOT
	@cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	@cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	@cp initrd/initrd.tar iso_root/boot/
	@echo "[ISO] $(IMAGE_NAME).iso"
	@xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso 2>/dev/null
	@rm -rf iso_root
endef

.PHONY: initrd
initrd: $(KERNEL_BIN) $(KERNEL_SYM) $(PROC_VERSION_FILE) userprogs
	$(build_initrd_tar)

$(IMAGE_NAME).iso: limine/limine $(KERNEL_BIN) initrd
	$(build_iso)

# without cloning/pulling/building the user programs.
.PHONY: repack
repack: limine/limine $(KERNEL_BIN) $(KERNEL_SYM) $(PROC_VERSION_FILE) initrd-files
	$(build_initrd_tar)
	$(build_iso)

# homebrew qemu has no sdl, so only use it on mac if the qemu on PATH was built with it
QEMU_MAC_DISPLAY := $(if $(shell qemu-system-x86_64 -display help 2>/dev/null | grep -x sdl),sdl,cocoa)

ifeq ($(UNAME_S),Darwin)
# coreaudio only keeps 4 buffers of ~11ms by default, less than qemu stalls for when the
# window is busy (moving the mouse), so it ran dry and stuttered. 10 is ~116ms
QEMU_AUDIODEV ?= coreaudio,out.buffer-count=10
else
QEMU_AUDIODEV ?= none
endif
QEMU_AUDIO := -audiodev $(QEMU_AUDIODEV),id=snd0 -device AC97,audiodev=snd0

ifeq ($(UNAME_S)$(UNAME_M),Darwinarm64)
QEMU_HOST := -accel tcg,thread=multi,tb-size=1024 -cpu max -display $(QEMU_MAC_DISPLAY)
else
QEMU_HOST := --enable-kvm -display sdl
endif

USB ?= ohci
ifeq ($(USB),xhci)
QEMU_USB_HC := -device qemu-xhci,id=usb
else ifeq ($(USB),ohci)
QEMU_USB_HC := -device pci-ohci,id=usb
else
$(error USB=$(USB) isnt a thing, use USB=ohci or USB=xhci)
endif
QEMU_USB := $(QEMU_USB_HC) -device usb-kbd,bus=usb.0 -device usb-mouse,bus=usb.0 \
            -drive file=$(USB_DISK),format=raw,if=none,id=usbstick \
            -device usb-storage,bus=usb.0,drive=usbstick

# asking for a controller by name means you want to test it, so no PS/2 to fall back on
ifneq ($(filter command line environment,$(origin USB)),)
QEMU_USB += -machine pc,i8042=off
endif

LOG_SOCK ?= /tmp/bleed-com3-$(shell id -u).sock
QEMU_SERIAL_SETUP = rm -f "$(LOG_SOCK)"; serial="-serial mon:stdio"; \
	if [ "$(LOG)" != shared ] && sh tools/logterm.sh "$(LOG_SOCK)"; then \
		serial="$$serial -serial null -serial unix:$(LOG_SOCK),server=on,wait=on"; \
	fi;

define run_qemu
	$(QEMU_SERIAL_SETUP) qemu-system-x86_64 \
		-name "$(QEMU_NAME)" \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMU_HOST) \
		-boot d \
		-m $(MEMSZ) \
		$$serial \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0 \
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0 \
		$(QEMU_AUDIO) \
		$(QEMU_USB) \
		$(1)
endef

# what an intel mac looks like to us: no PS/2 controller at all, the keyboard and trackpad hang off the usb root ports
QEMU_OHCI_INPUT := -machine pc,i8042=off

.PHONY: run
run: $(IMAGE_NAME).iso $(DISKS)
	$(run_qemu)

# no window, the desktop still runs but the way in is the shell on this terminal. ctrl+a x quits
.PHONY: run-headless
run-headless: $(IMAGE_NAME).iso $(DISKS)
	$(call run_qemu,-display none)

# same as run but the only keyboard and mouse are the usb ones, for testing the usb stack (USB=xhci works here too)
.PHONY: run-ohci
run-ohci: $(IMAGE_NAME).iso $(DISKS)
	$(call run_qemu,$(QEMU_OHCI_INPUT))

# boot the existing iso as-is: no rebuild, no clone or pull, pair with `make repack` to avoid git entirely
.PHONY: runlocal
runlocal: $(DISKS)
	@if [ ! -f $(IMAGE_NAME).iso ]; then \
		echo "ERROR: $(IMAGE_NAME).iso not found, run 'make repack' first"; \
		exit 1; \
	fi
	$(run_qemu)

.PHONY: run-uefi
run-uefi: $(IMAGE_NAME).iso $(DISKS)
	$(QEMU_SERIAL_SETUP) qemu-system-x86_64 \
		-name "$(QEMU_NAME)" \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_FW) \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMU_HOST) \
		-boot d \
		-m $(MEMSZ) \
		$$serial \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0 \
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0 \
		$(QEMU_AUDIO) \
		$(QEMU_USB)

#apple silicon emulation sucks, if you are getting performance issues, please test it on an x86_64 machine before submitting an issue!
#SMAP and SMEP are known to be funny on mac, it may not emulate it at all, just be careful but bleed should handle it.
.PHONY: run-mac
run-mac: $(IMAGE_NAME).iso $(DISKS)
	$(QEMU_SERIAL_SETUP) qemu-system-x86_64 \
		-name "$(QEMU_NAME)" \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_FW) \
		-cdrom $(IMAGE_NAME).iso \
		-accel tcg,thread=multi,tb-size=1024 \
		-cpu max \
		-m $(MEMSZ) \
		-boot d \
		$$serial \
		-display $(QEMU_MAC_DISPLAY) \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0 \
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0 \
		$(QEMU_AUDIO) \
		$(QEMU_USB)

define create_ext2_disk
    @echo "[DISK] Creating $(1) ($(2))"
    $(eval TMP_DIR := .tmp_dir_$(1))
    $(eval TMP_IMG := .tmp_img_$(1))
    @export PATH=$$PATH:/sbin:/usr/sbin:/opt/homebrew/opt/e2fsprogs/sbin:/usr/local/opt/e2fsprogs/sbin; \
    if ! command -v mkfs.ext2 > /dev/null 2>&1; then \
        echo "[DISK] ERROR: mkfs.ext2 not found. On macOS: brew install e2fsprogs"; \
        exit 1; \
    fi; \
    dd if=/dev/zero of=$(1) bs=1M count=$(or $(4),$(DISK_SIZE_MB)) status=none; \
    if [ "$(2)" = "gpt" ]; then \
        sgdisk -n 1:2048:0 -t 1:8300 $(1) > /dev/null 2>&1; \
    elif [ "$$(uname)" = "Darwin" ]; then \
        echo "2048,$$(($(or $(4),$(DISK_SIZE_MB)) * 2048 - 2048)),0x83,-" | fdisk -y -r $(1) > /dev/null 2>&1; \
    else \
        { printf 'o\nn\np\n1\n2048\n\nw\n'; } | fdisk $(1) > /dev/null 2>&1; \
    fi; \
    mkdir -p $(TMP_DIR); \
    echo "$(3)" > $(TMP_DIR)/hello.txt; \
    if [ -n "$(5)" ] && [ -d "$(5)" ]; then COPYFILE_DISABLE=1 cp -R "$(5)"/. $(TMP_DIR)/; fi; \
    truncate -s $$(($(or $(4),$(DISK_SIZE_MB)) - 1))M $(TMP_IMG); \
    mkfs.ext2 -F -d $(TMP_DIR) $(TMP_IMG); \
    dd if=$(TMP_IMG) of=$(1) bs=1M seek=1 conv=notrunc status=none; \
    rm -rf $(TMP_IMG) $(TMP_DIR)
endef

define create_exfat_disk
    @echo "[DISK] Creating $(1) ($(2)) [exfat]"
	$(eval TMP_DIR := .tmp_dir_$(1))
	$(eval TMP_IMG := .tmp_img_$(1))
    @export PATH=$$PATH:/sbin:/usr/sbin; \
    dd if=/dev/zero of=$(1) bs=1M count=$(DISK_SIZE_MB) status=none; \
    if [ "$(2)" = "gpt" ]; then \
        sgdisk -n 1:2048:0 -t 1:0700 $(1) > /dev/null 2>&1; \
    elif [ "$$(uname)" = "Darwin" ]; then \
        echo "2048,$$(($(DISK_SIZE_MB) * 2048 - 2048)),0x07,-" | fdisk -y -r $(1) > /dev/null 2>&1; \
    else \
        { printf 'o\nn\np\n1\n2048\n\nt\n7\nw\n'; } | fdisk $(1) > /dev/null 2>&1; \
    fi; \
    mkdir -p $(TMP_DIR); \
    echo "$(3)" > $(TMP_DIR)/hello.txt; \
    truncate -s $$(($(DISK_SIZE_MB) - 1))M $(TMP_IMG); \
    if [ "$$(uname)" = "Darwin" ]; then \
        dev=$$(hdiutil attach -nomount $(TMP_IMG) | tr -d '[:space:]'); \
        newfs_exfat -v BLEEDFS "$$dev" > /dev/null; \
        diskutil mount "$$dev" > /dev/null; \
        mnt=$$(diskutil info "$$dev" | awk -F': *' '/Mount Point/{print $$2}'); \
        COPYFILE_DISABLE=1 cp $(TMP_DIR)/hello.txt "$$mnt/hello.txt"; \
        diskutil unmount "$$dev" > /dev/null; \
        hdiutil detach "$$dev" > /dev/null; \
    else \
        mkfs.exfat -L BLEEDFS $(TMP_IMG) > /dev/null 2>&1; \
        MTOOLS_SKIP_CHECK=1 mcopy -i $(TMP_IMG) $(TMP_DIR)/hello.txt ::hello.txt; \
    fi; \
    dd if=$(TMP_IMG) of=$(1) bs=1M seek=1 conv=notrunc status=none; \
    rm -rf $(TMP_IMG) $(TMP_DIR)
endef

$(IDE_DISK):
	$(call create_ext2_disk,$@,msdos,if your seeing this the ide driver works which is awesome)

$(SATA_DISK):
	$(call create_exfat_disk,$@,gpt,if your seeing this the sata driver AND exfat both work which is even more awesome)

$(NVME_DISK):
	@rm -rf $(NVME_FILES) && mkdir -p $(NVME_FILES)
	@for dir in $(DISK_PROGRAM_DIRS); do \
		grep -q '^disk:' "$$dir/Makefile" 2>/dev/null || continue; \
		echo "[DISK] Getting $${dir##*/}'s files for $@"; \
		$(MAKE) -s -C "$$dir" disk DISK=$(abspath $(NVME_FILES)) || exit 1; \
	done
	$(call create_ext2_disk,$@,gpt,if your seeing this the nvme driver works which is the most awesomest,$(NVME_DISK_SIZE_MB),$(NVME_FILES))
	@rm -rf $(NVME_FILES)

$(USB_DISK):
	$(call create_ext2_disk,$@,msdos,if your seeing this usb mass storage works and we can finally move files around)

.PHONY: clean
clean:
	rm -rf bin $(IMAGE_NAME).iso iso_root $(DISKS)
	rm -f $(PROC_VERSION_FILE)
	rm -f $(KERNEL_SYM)
	find kernel klibc -name '*.o' -delete
	find kernel klibc -name '*.d' -delete
	find initrd -name '*.tar' -delete
	find initrd -name '._*' -delete
	find initrd -name '.DS_Store' -delete
	rm -rf limine

.PHONY: FORCE
FORCE:

distclean:
	rm -rf bin $(IMAGE_NAME).iso iso_root $(DISKS)
	rm -rf initrd/lib initrd/include
	rm -f $(PROC_VERSION_FILE)
	rm -f $(KERNEL_SYM)
	@done_dirs=""; for entry in $(USER_REPOS); do \
		repo=$${entry%% *}; \
		[ -n "$(USERPROG_SRC)" ] || break; \
		local_dir=$$(find "$(USERPROG_SRC)" -maxdepth 1 -type d -iname "$${repo##*/}" | head -n 1); \
		[ -n "$$local_dir" ] || continue; \
		case " $$done_dirs " in *" $$local_dir "*) continue;; esac; \
		done_dirs="$$done_dirs $$local_dir"; \
		echo "[USER] distclean $$local_dir"; \
		$(MAKE) -s -C "$$local_dir" distclean; \
	done
	@if [ -n "$(USERPROG_SRC)" ] && [ -d "$(USERPROG_SRC)/blibc" ]; then \
		echo "[BLIBC] clean $(USERPROG_SRC)/blibc"; \
		$(MAKE) -s -C "$(USERPROG_SRC)/blibc" clean; \
	fi
	find kernel klibc -name '*.o' -delete
	find kernel klibc -name '*.d' -delete
	rm -rf $(USER_BIN_DIR)
	rm -rf initrd/bin/*
	find initrd -name '*.tar' -delete
	find initrd -name '._*' -delete
	find initrd -name '.DS_Store' -delete
