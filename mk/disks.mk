NVME_DISK := nvmedisk.img
USB_DISK  := $(if $(filter ext2,$(USB_FS)),usbdisk.img,usbdisk-$(USB_FS).img)
EMMC_DISK := emmcdisk.img

ifeq ($(filter ext2 ext3 ext4,$(USB_FS)),)
$(error USB_FS=$(USB_FS) isnt a thing, use USB_FS=ext2, ext3 or ext4)
endif
DISKS     := $(NVME_DISK) $(USB_DISK) $(if $(EMMC),$(EMMC_DISK))

NVME_FILES := .nvme_files

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
    src=$(TMP_DIR); \
    if [ -n "$(5)" ] && [ -d "$(5)" ]; then src="$(5)"; fi; \
    mkdir -p "$$src"; \
    echo "$(3)" > "$$src"/hello.txt; \
    truncate -s $$(($(or $(4),$(DISK_SIZE_MB)) - 1))M $(TMP_IMG); \
    mkfs.$(or $(7),ext2) -F $(6) -d "$$src" $(TMP_IMG); \
    dd if=$(TMP_IMG) of=$(1) bs=1M seek=1 conv=notrunc status=none; \
    rm -rf $(TMP_IMG) $(TMP_DIR)
endef


$(NVME_DISK): $(PKG_SUM)
	@rm -rf $(NVME_FILES) && mkdir -p $(NVME_FILES)/bpkg
	@for f in $(PKG_OUT)/*.bpkg; do [ -e "$$f" ] && { ln "$$f" $(NVME_FILES)/bpkg/ 2>/dev/null || cp "$$f" $(NVME_FILES)/bpkg/; }; done
	$(call create_ext2_disk,$@,gpt,if your seeing this the nvme driver works which is the most awesomest,$$(( $$(cat /dev/null $(PKG_OUT)/*.bpkg 2>/dev/null | wc -c) / 1048576 * 4 + $(DISK_SIZE_MB) )),$(NVME_FILES),-b 4096)
	@rm -rf $(NVME_FILES)

$(EMMC_DISK):
	$(call create_ext2_disk,$@,gpt,if your seeing this the emmc driver works and the chromebook might have a disk soon)

$(USB_DISK):
	$(call create_ext2_disk,$@,msdos,if your seeing this usb mass storage works and we can finally move files around,,,,$(USB_FS))