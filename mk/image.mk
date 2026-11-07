IMAGE_NAME := bleed-kernel
ISO        := $(IMAGE_NAME).iso

LIMINE_VERSION := 12.9.2
LIMINE_URL     := https://github.com/Limine-Bootloader/Limine/releases/download/v$(LIMINE_VERSION)/limine-binary.tar.gz
LIMINE_SHA256  := 047e7eeae7de4de62fca4fc3740b621aa37a2038b208f4d0ac993750a18eed86

# the binary release has no limine tool in it
LIMINE := limine/BOOTX64.EFI

$(LIMINE):
	rm -rf limine
	mkdir -p limine
	curl -fsSL -o limine/limine-binary.tar.gz $(LIMINE_URL)
	echo "$(LIMINE_SHA256)  limine/limine-binary.tar.gz" | shasum -a 256 -c -
	tar -xzf limine/limine-binary.tar.gz -C limine --strip-components=1
	rm -f limine/limine-binary.tar.gz

define build_initrd_tar
	@sh tools/mkinitrd.sh $(INITRD_ASSETS)
endef

PKG_OUT   := .packages
PKG_STAGE := .pkg_stage
PKG_SUM   := .packages.sum
MKBPKG    := $(firstword $(wildcard $(USERPROG_SRC)/Bleed-BPM/mkbpkg.sh) $(USER_BIN_DIR)/bpm/mkbpkg.sh)

.PHONY: packages
packages: $(PKG_SUM)

ifeq ($(NO_BUILD),)
$(PKG_SUM): userprogs
	@mkdir -p $(PKG_OUT)
	@$(if $(BPKGS),MKBPKG="$(abspath $(MKBPKG))" PKG_STAGE="$(abspath $(PKG_STAGE))" PKG_ASSETS="$(PKG_ASSETS)" \
		$(foreach p,$(BPKGS),DESC_$(p)="$(DESC_$(p))" EXEC_$(p)="$(EXEC_$(p))") \
		$(USERPROGS_SH) bpkg $(abspath $(PKG_OUT)) $(call program_entries,$(BPKGS)),rm -f $(PKG_OUT)/*.bpkg)
	@# names, sizes and dates, not link counts, the disks hard link these
	@sum=$$(ls -ln $(PKG_OUT) | grep '\.bpkg$$' | awk '{ $$2 = ""; print }' | cksum); \
	[ "$$sum" = "$$(cat $@ 2>/dev/null)" ] || echo "$$sum" > $@
	@echo "[BPKG] packages: $$(cd $(PKG_OUT) && ls *.bpkg 2>/dev/null | tr '\n' ' ')"
else
$(PKG_SUM):
endif

# data.img is the packages as their own download next to the iso, an ext2 image labelled BLEEDDATA.
# put in the top of any drive the kernel mounts (the usb stick the iso is on) its mounted at
# /mnt/data (fs/data_image.c), limine never sees it. only made for PACKAGE, the nvme disk has
# the packages for testing
DATA_DIR := .data_img
DATA_IMG := data.img
ISO_DATA := $(if $(PACKAGE),$(if $(BPKGS),$(DATA_IMG)))

.PHONY: data-img
data-img: $(DATA_IMG)

# hard links, a copy of the sparse image crawls on macos
$(DATA_IMG): $(PKG_SUM)
	@echo "[DATA] $(DATA_IMG)"
	@rm -rf $(DATA_DIR) && mkdir -p $(DATA_DIR)/bpkg
	@for f in $(PKG_OUT)/*.bpkg; do [ -e "$$f" ] && { ln "$$f" $(DATA_DIR)/bpkg/ 2>/dev/null || cp "$$f" $(DATA_DIR)/bpkg/; }; done
	@export PATH=$$PATH:/sbin:/usr/sbin:/opt/homebrew/opt/e2fsprogs/sbin:/usr/local/opt/e2fsprogs/sbin; \
	if ! command -v mkfs.ext2 > /dev/null 2>&1; then \
		echo "[DATA] ERROR: mkfs.ext2 not found. On macOS: brew install e2fsprogs"; \
		exit 1; \
	fi; \
	rm -f $(DATA_IMG); \
	size=$$(( $$(du -sm $(DATA_DIR) | cut -f1) * 11 / 10 + $(DATA_SPARE_MB) )); \
	truncate -s $${size}M $(DATA_IMG) && \
	mkfs.ext2 -q -F -L BLEEDDATA -d $(DATA_DIR) $(DATA_IMG)
	@rm -rf $(DATA_DIR)

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
	@$(if $(ISO_DATA),ln $(DATA_IMG) iso_root/ 2>/dev/null || cp $(DATA_IMG) iso_root/)
	@echo "[ISO] $(ISO)$(if $(ISO_DATA), with $(DATA_IMG) packaged)"
	@xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		iso_root -o $(ISO) 2>/dev/null
	@rm -rf iso_root
endef

.PHONY: initrd
initrd: $(KERNEL_BIN) $(KERNEL_SYM) $(KERNEL_EXPORTS) $(PROC_VERSION_FILE) userprogs drivers
	$(build_initrd_tar)

$(ISO): $(LIMINE) $(KERNEL_BIN) initrd $(ISO_DATA)
	$(build_iso)
