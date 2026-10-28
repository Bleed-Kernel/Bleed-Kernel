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

# DATA_PROGRAMS from the profile go in data.img, an ext2 image labelled BLEEDDATA thats its own
# download next to the iso. put in the top of any drive the kernel mounts (the usb stick the iso
# is on) its mounted at /mnt/data (fs/data_image.c), limine never sees it
DATA_DIR     := .data_img
DATA_IMG     := data.img
PROFILE_DATA := $(filter $(DATA_PROGRAMS),$(PROGRAMS))

DATA_SUM     := .data_img.sum

# the copy runs every build but data.img is only touched when whats going in it changed, so the nvme
# disk that holds it isnt remade for nothing. the staging folder is kept so the game copy is just the changes
.PHONY: data-img
data-img: $(DATA_IMG)

ifeq ($(NO_BUILD),)
$(DATA_IMG): userprogs
	@mkdir -p $(DATA_DIR)
	@for d in $(DATA_DIR)/*; do \
		[ -e "$$d" ] || continue; \
		case " $(PROFILE_DATA) " in *" $${d##*/} "*) ;; *) rm -rf "$$d" ;; esac; \
	done
	@DATA_ASSETS="$(DATA_ASSETS)" $(USERPROGS_SH) disk $(abspath $(DATA_DIR)) $(call program_entries,$(PROFILE_DATA))
	@sum=$$(cd $(DATA_DIR) && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 cksum | cksum); \
	if [ -f $(DATA_IMG) ] && [ "$$sum" = "$$(cat $(DATA_SUM) 2>/dev/null)" ]; then \
		echo "[DATA] $(DATA_IMG) is up to date"; \
	else \
		echo "[DATA] $(DATA_IMG)"; \
		export PATH=$$PATH:/sbin:/usr/sbin:/opt/homebrew/opt/e2fsprogs/sbin:/usr/local/opt/e2fsprogs/sbin; \
		if ! command -v mkfs.ext2 > /dev/null 2>&1; then \
			echo "[DATA] ERROR: mkfs.ext2 not found. On macOS: brew install e2fsprogs"; \
			exit 1; \
		fi; \
		rm -f $(DATA_IMG); \
		size=$$(( $$(du -sm $(DATA_DIR) | cut -f1) * 11 / 10 + $(DATA_SPARE_MB) )); \
		truncate -s $${size}M $(DATA_IMG) && \
		mkfs.ext2 -q -F -L BLEEDDATA -d $(DATA_DIR) $(DATA_IMG) && \
		echo "$$sum" > $(DATA_SUM); \
	fi
else
$(DATA_IMG):
endif

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
	@echo "[ISO] $(ISO)"
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
initrd: $(KERNEL_BIN) $(KERNEL_SYM) $(PROC_VERSION_FILE) userprogs
	$(build_initrd_tar)

$(ISO): $(LIMINE) $(KERNEL_BIN) initrd
	$(build_iso)
