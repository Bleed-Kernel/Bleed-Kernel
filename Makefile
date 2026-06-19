export MAKEFLAGS = -j8

IMAGE_NAME := bleed-kernel
OBJDIR := bin/obj
KERNEL_BIN := bin/bleed-kernel
OVMF_FW := edk2-ovmf/OVMF-pure-efi.fd
MEMSZ := 512M
PROC_VERSION_FILE := initrd/proc/version
BUILD_GIT_HASH := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
BUILD_GIT_COUNT := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
BUILD_GIT_DIRTY := $(shell if git diff --quiet --ignore-submodules HEAD >/dev/null 2>&1; then echo ""; else echo "-dirty"; fi)
BUILD_VERSION := r$(BUILD_GIT_COUNT)-g$(BUILD_GIT_HASH)$(BUILD_GIT_DIRTY)

MAKEFLAGS += --no-print-directory

IDE_DISK := idedisk.img
SATA_DISK := satadisk.img
NVME_DISK := nvmedisk.img
DISK_SIZE_MB := 128

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
    "https://github.com/Bleed-Kernel/Bleed-Taskman taskman" \
    "https://github.com/Bleed-Kernel/Bleed-tvi tvi" \
    "https://github.com/Bleed-Kernel/Bleed-Coreutils cat" \
    "https://github.com/Bleed-Kernel/Bleed-Coreutils echo" \

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
	git clone https://github.com/limine-bootloader/limine limine --branch v10.5.0-binary --depth 1
	cd limine && git checkout $(LIMINE_10_5_0)

.PHONY: userprogs
userprogs:
	@mkdir -p $(USER_BIN_DIR)
	@mkdir -p $(INITRD_BIN)
	@set -e; for entry in $(USER_REPOS); do \
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
		$(MAKE) -s -C "$$dir" blibc $(USERPROG_TOOLCHAIN); \
		echo "[USER] Building $$name"; \
		$(MAKE) -s -C "$$dir" $(USERPROG_TOOLCHAIN); \
		if [ -f "$$dir/bin/$$name" ]; then \
			COPYFILE_DISABLE=1 cp "$$dir/bin/$$name" $(INITRD_BIN)/$$name; \
		else \
			echo "ERROR: $$name binary not found"; \
			exit 1; \
		fi; \
	done

.PHONY: initrd
initrd: $(KERNEL_BIN) $(KERNEL_SYM) $(PROC_VERSION_FILE) userprogs
	@mkdir -p initrd
	@find initrd -name '._*' -delete
	@find initrd -name '.DS_Store' -delete
	@COPYFILE_DISABLE=1 tar -cf initrd/initrd.tar initrd/*/* initrd/*.*

$(IMAGE_NAME).iso: limine/limine $(KERNEL_BIN) initrd
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

.PHONY: run
run: $(IMAGE_NAME).iso $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		$(if $(filter Darwinarm64,$(UNAME_S)$(UNAME_M)), \
			-accel tcg -cpu max -display cocoa, \
			--enable-kvm -cpu host -display sdl) \
		-boot d \
		-m $(MEMSZ) \
		-serial stdio \
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
		-cdrom $(IMAGE_NAME).iso \
		$(if $(filter Darwinarm64,$(UNAME_S)$(UNAME_M)), \
			-accel tcg -cpu max -display cocoa, \
			--enable-kvm -cpu host -display sdl) \
		-boot d \
		-m $(MEMSZ) \
		-serial stdio \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0 \
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0

.PHONY: run-mac
run-mac: $(IMAGE_NAME).iso $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_FW) \
		-cdrom $(IMAGE_NAME).iso \
		-accel tcg \
		-cpu max \
		-m $(MEMSZ) \
		-boot d \
		-serial stdio \
		-display cocoa \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0 \
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0

define create_ext2_disk
    @echo "[DISK] Creating $(1) ($(2))"
    $(eval TMP_DIR := .tmp_dir_$(1))
    $(eval TMP_IMG := .tmp_img_$(1))
    @export PATH=$$PATH:/sbin:/usr/sbin; \
    dd if=/dev/zero of=$(1) bs=1M count=$(DISK_SIZE_MB) status=none; \
    if [ "$(2)" = "gpt" ]; then \
        sgdisk -n 1:2048:0 -t 1:8300 $(1) > /dev/null 2>&1; \
    else \
        { printf 'o\nn\np\n1\n2048\n\nw\n'; } | fdisk $(1) > /dev/null 2>&1; \
    fi; \
    mkdir -p $(TMP_DIR); \
    echo "$(3)" > $(TMP_DIR)/hello.txt; \
    truncate -s $$(($(DISK_SIZE_MB) - 1))M $(TMP_IMG); \
    mkfs.ext2 -F -d $(TMP_DIR) $(TMP_IMG) > /dev/null 2>&1; \
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
	$(call create_ext2_disk,$@,gpt,if your seeing this the nvme driver works which is the most awesomest)

.PHONY: clean
clean:
	rm -rf bin $(IMAGE_NAME).iso iso_root $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)
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
	rm -rf bin $(IMAGE_NAME).iso iso_root $(IDE_DISK) $(SATA_DISK) $(NVME_DISK)
	rm -f $(PROC_VERSION_FILE)
	find kernel klibc -name '*.o' -delete
	find kernel klibc -name '*.d' -delete
	rm -rf $(USER_BIN_DIR)
	rm -rf initrd/bin/*
	find initrd -name '*.tar' -delete
	find initrd -name '._*' -delete
	find initrd -name '.DS_Store' -delete