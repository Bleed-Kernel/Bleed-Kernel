export MAKEFLAGS = -j8

IMAGE_NAME := bleed-kernel
OBJDIR := bin/obj
KERNEL_BIN := bin/bleed-kernel
OVMF_FW := edk2-ovmf/OVMF-pure-efi.fd
MEMSZ := 3G
PROC_VERSION_FILE := initrd/proc/version
BUILD_GIT_HASH := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
BUILD_GIT_COUNT := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
BUILD_GIT_DIRTY := $(shell if git diff --quiet --ignore-submodules HEAD >/dev/null 2>&1; then echo ""; else echo "-dirty"; fi)
BUILD_VERSION := r$(BUILD_GIT_COUNT)-g$(BUILD_GIT_HASH)$(BUILD_GIT_DIRTY)

IDE_DISK := idedisk.img
SATA_DISK := satadisk.img
NVME_DISK := nvmedisk.img
DISK_SIZE_MB := 128

CC := cc
LD := ld
NM := nm

CFLAGS := -g -O2 -Wall -Werror -Wextra -std=gnu11 \
          -nostdinc -ffreestanding -fno-stack-protector \
          -fno-stack-check -fno-lto -fno-PIC -fno-pie \
          -ffunction-sections -fdata-sections -fno-omit-frame-pointer \
          -m64 -march=x86-64 -mabi=sysv -mno-80387 -mno-red-zone \
          -mcmodel=kernel -I kernel/include -I klibc/include \
          -MMD -MP -mavx2 -fvect-cost-model=dynamic

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
    "https://codeberg.org/Bleed-Kernel/Verdict-Shell verdict" \
    "https://codeberg.org/Bleed-Kernel/Bleed-Doom doom" \
    "https://codeberg.org/Bleed-Kernel/Bleed-Taskman taskman" \
    "https://codeberg.org/Bleed-Kernel/Bleed-tvi tvi" \
    "https://codeberg.org/Bleed-Kernel/Bleed-Coreutils cat" \
    "https://codeberg.org/Bleed-Kernel/Bleed-Coreutils echo" \

USER_BIN_DIR := external/
INITRD_BIN := initrd/bin

.PHONY: all
all: $(IMAGE_NAME).iso $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)

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
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJ) -o $@

$(MK_SYMTAB): tools/mksymtab.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -std=gnu11 $< -o $@

$(KERNEL_NM): $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	$(NM) -n $(KERNEL_BIN) > $@

$(KERNEL_SYM): $(KERNEL_NM) $(MK_SYMTAB)
	@mkdir -p $(dir $@)
	$(MK_SYMTAB) $(KERNEL_NM) $@

limine/limine:
	rm -rf limine
	git clone https://codeberg.org/Limine/Limine.git limine --branch v10.5.0-binary --depth 1
	cd limine && git checkout $(LIMINE_10_5_0)

.PHONY: userprogs
userprogs:
	@mkdir -p $(USER_BIN_DIR)
	@mkdir -p $(INITRD_BIN)
	@for entry in $(USER_REPOS); do \
		repo=$${entry%% *}; \
		name=$${entry##* }; \
		dir=$(USER_BIN_DIR)/$$name; \
		if [ ! -d "$$dir" ]; then \
			echo "[USER] Cloning $$name from $$repo"; \
			git clone "$$repo" "$$dir"; \
		else \
			echo "[USER] Pulling latest for $$name"; \
			(cd "$$dir" && git pull --rebase); \
		fi; \
		echo "[USER] Preparing blibc for $$name"; \
		$(MAKE) -C "$$dir" blibc; \
		echo "[USER] Building $$name"; \
		$(MAKE) -C "$$dir"; \
		if [ -f "$$dir/bin/$$name" ]; then \
			cp "$$dir/bin/$$name" $(INITRD_BIN)/$$name; \
		else \
			echo "ERROR: $$name binary not found"; \
			exit 1; \
		fi; \
	done

.PHONY: initrd
initrd: $(KERNEL_BIN) $(KERNEL_SYM) $(PROC_VERSION_FILE) userprogs
	@mkdir -p initrd
	tar -cf initrd/initrd.tar initrd/*/* initrd/*.*

$(IMAGE_NAME).iso: limine/limine $(KERNEL_BIN) initrd
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v $(KERNEL_BIN) iso_root/boot/
	cp -v wallpaper.png iso_root/
	mkdir -p iso_root/boot/limine
	cp -v limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	cp -v initrd/initrd.tar iso_root/boot/
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	rm -rf iso_root

.PHONY: run
run: $(IMAGE_NAME).iso $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)
	qemu-system-x86_64 \
		--cdrom $(IMAGE_NAME).iso \
		--enable-kvm \
		-cpu host \
		-boot d \
		-m $(MEMSZ) \
		-serial stdio \
		-display sdl \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0 \
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0

.PHONY: run-uefi
run-uefi: $(IMAGE_NAME).iso $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_FW) \
		--cdrom $(IMAGE_NAME).iso \
		--enable-kvm \
		-cpu host \
		-boot d \
		-m $(MEMSZ) \
		-serial stdio \
		-display sdl \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0

$(IDE_DISK):
	@echo "[DISK] Creating $(IDE_DISK) ($(DISK_SIZE_MB)MB)"
	@dd if=/dev/zero of=$(IDE_DISK) bs=1M count=$(DISK_SIZE_MB) > /dev/null 2>&1
	@echo "[DISK] Partitioning IDE (MBR)..."
	@parted -s $(IDE_DISK) mklabel msdos mkpart primary fat32 1MiB 100%
	@echo "[DISK] Formatting IDE FAT32 partition..."
	@mformat -i $(IDE_DISK)@@1M -F ::
	@echo "[DISK] Adding hello.txt to IDE"
	@echo "if your seeing this, the ide driver works, which is awesome" > .hello_ide.txt
	@mcopy -i $(IDE_DISK)@@1M .hello_ide.txt ::/hello.txt
	@rm .hello_ide.txt

$(SATA_DISK):
	@echo "[DISK] Creating $(SATA_DISK) ($(DISK_SIZE_MB)MB)"
	@dd if=/dev/zero of=$(SATA_DISK) bs=1M count=$(DISK_SIZE_MB) > /dev/null 2>&1
	@echo "[DISK] Partitioning SATA (GPT)..."
	@parted -s $(SATA_DISK) mklabel gpt mkpart primary fat32 1MiB 100%
	@echo "[DISK] Formatting SATA FAT32 partition..."
	@mformat -i $(SATA_DISK)@@1M -F ::
	@echo "[DISK] Adding hello.txt to SATA"
	@echo "if your seeing this, the sata driver works, which is even more awesome" > .hello_sata.txt
	@mcopy -i $(SATA_DISK)@@1M .hello_sata.txt ::/hello.txt
	@rm .hello_sata.txt

$(NVME_DISK):
	@echo "[DISK] Creating $(NVME_DISK) ($(DISK_SIZE_MB)MB)"
	@dd if=/dev/zero of=$(NVME_DISK) bs=1M count=$(DISK_SIZE_MB) > /dev/null 2>&1
	@echo "[DISK] Partitioning NVMe (GPT)..."
	@parted -s $(NVME_DISK) mklabel gpt mkpart primary fat32 1MiB 100%
	@echo "[DISK] Formatting NVMe FAT32 partition..."
	@mformat -i $(NVME_DISK)@@1M -F ::
	@echo "[DISK] Adding hello.txt to NVMe"
	@echo "if your seeing this, the nvme driver works, which is the most awesomest" > .hello_nvme.txt
	@mcopy -i $(NVME_DISK)@@1M .hello_nvme.txt ::/hello.txt
	@rm .hello_nvme.txt

.PHONY: clean
clean:
	rm -rf bin $(IMAGE_NAME).iso iso_root $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)
	rm -f $(PROC_VERSION_FILE)
	rm -f $(KERNEL_SYM)
	find kernel klibc -name '*.o' -delete
	find kernel klibc -name '*.d' -delete
	find initrd -name '*.tar' -delete
	rm -rf limine

.PHONY: FORCE
FORCE:

distclean:
	rm -rf bin $(IMAGE_NAME).iso iso_root $(IDE_DISK) $(SATA_DISK)
	rm -f $(PROC_VERSION_FILE)
	find kernel klibc -name '*.o' -delete
	find kernel klibc -name '*.d' -delete
	rm -rf $(USER_BIN_DIR)
	rm -rf initrd/bin/*
	find initrd -name '*.tar' -delete