IDE_DISK  := idedisk.img
SATA_DISK := satadisk.img
NVME_DISK := nvmedisk.img
USB_DISK  := usbdisk.img
DISKS     := $(IDE_DISK) $(SATA_DISK) $(NVME_DISK) $(USB_DISK)

NVME_FILES         := .nvme_files
NVME_DISK_PROGRAMS := $(if $(NO_BUILD),,$(filter $(DISK_PROGRAMS),$(PROGRAMS)))
NVME_DISK_SIZE_MB  := $(if $(NVME_DISK_PROGRAMS),$(NVME_BIG_MB),$(DISK_SIZE_MB))
NVME_TOO_SMALL     := $(shell [ -f $(NVME_DISK) ] && [ $$(wc -c < $(NVME_DISK)) -lt $$(($(NVME_DISK_SIZE_MB) * 1048576)) ] && echo yes)

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

# this one on a release profile will put half life in here
$(NVME_DISK): $(if $(NVME_TOO_SMALL),FORCE) | $(if $(NVME_DISK_PROGRAMS),userprogs)
	@rm -rf $(NVME_FILES) && mkdir -p $(NVME_FILES)
	@$(USERPROGS_SH) disk $(abspath $(NVME_FILES)) $(call program_entries,$(NVME_DISK_PROGRAMS))
	$(call create_ext2_disk,$@,gpt,if your seeing this the nvme driver works which is the most awesomest,$(NVME_DISK_SIZE_MB),$(NVME_FILES))
	@rm -rf $(NVME_FILES)

$(USB_DISK):
	$(call create_ext2_disk,$@,msdos,if your seeing this usb mass storage works and we can finally move files around)
