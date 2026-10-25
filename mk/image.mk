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
