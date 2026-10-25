# the bleed kernel. this file decides what gets built, the how lives in mk/ and the knobs in config.mk
# `make help` for the rundown

include config.mk
-include config.local.mk

export MAKEFLAGS = -j$(JOBS)
MAKEFLAGS += --no-print-directory

PROFILES     := DEV MINIMAL RELEASE
PROFILE_GOAL := $(filter $(PROFILES),$(MAKECMDGOALS))
PROFILE      ?= $(or $(PROFILE_GOAL),$(DEFAULT_PROFILE))

ifneq ($(word 2,$(PROFILE_GOAL)),)
$(error pick one profile, not $(PROFILE_GOAL))
endif
ifeq ($(filter $(PROFILE),$(PROFILES)),)
$(error PROFILE=$(PROFILE) isnt a thing, use one of $(PROFILES))
endif

NO_BUILD := $(filter runlocal repack,$(MAKECMDGOALS))

include mk/programs.mk

# keep the build order from ALL_PROGRAMS whatever order the profile lists them in
PROGRAMS := $(filter $(PROGRAMS_$(PROFILE)),$(ALL_PROGRAMS))
UNKNOWN_PROGRAMS := $(filter-out $(ALL_PROGRAMS),$(PROGRAMS_$(PROFILE)))
ifneq ($(UNKNOWN_PROGRAMS),)
$(error PROGRAMS_$(PROFILE) has $(UNKNOWN_PROGRAMS) which arent in mk/programs.mk)
endif

include mk/toolchain.mk
include mk/kernel.mk
include mk/userprogs.mk
include mk/image.mk
include mk/disks.mk
include mk/qemu.mk

.DEFAULT_GOAL := build

.PHONY: build all
build: $(ISO) $(DISKS)
all: build

.PHONY: run
run: build
	$(run_qemu)

# no window, the desktop still runs but the way in is the shell on this terminal. ctrl+a x quits
.PHONY: run-headless
run-headless: build
	$(call run_qemu,-display none)

# same as run but the only keyboard and mouse are the usb ones, for testing the usb stack (USB=xhci works here too)
.PHONY: run-ohci
run-ohci: build
	$(call run_qemu,$(QEMU_OHCI_INPUT))

.PHONY: run-uefi
run-uefi: build
	$(call run_qemu,$(QEMU_UEFI))

.PHONY: run-mac
run-mac: QEMU_HOST := $(QEMU_HOST_MAC)
run-mac: build
	$(call run_qemu,$(QEMU_UEFI))

# boot the existing iso as-is: no rebuild, no clone or pull, pair with `make repack` to avoid git entirely
.PHONY: runlocal
runlocal: $(DISKS)
	@if [ ! -f $(ISO) ]; then \
		echo "ERROR: $(ISO) not found, run 'make build' first"; \
		exit 1; \
	fi
	$(run_qemu)

# tar up initrd/ as it is right now and put it in the iso.
.PHONY: repack
repack:
	@for f in $(KERNEL_BIN) $(LIMINE); do \
		if [ ! -e $$f ]; then echo "ERROR: $$f not found, run 'make build' first"; exit 1; fi; \
	done
	$(build_initrd_tar)
	$(build_iso)
.PHONY: clean
clean:
	rm -rf bin $(ISO) iso_root
	rm -f $(PROC_VERSION_FILE) $(KERNEL_SYM)
	find kernel klibc \( -name '*.o' -o -name '*.d' \) -delete
	find initrd \( -name '*.tar' -o -name '._*' -o -name '.DS_Store' \) -delete

.PHONY: distclean
distclean: clean
	rm -rf $(DISKS) $(NVME_FILES) limine $(USER_BIN_DIR)
	rm -rf initrd/lib initrd/include initrd/halflife $(INITRD_BIN)/*
	@$(USERPROGS_SH) distclean $(call program_entries,$(ALL_PROGRAMS))

.PHONY: help
help:
	@echo "make build [PROFILE]    build everything the profile wants"
	@echo "make run [PROFILE]      build then boot it, also run-headless run-ohci run-uefi run-mac"
	@echo "make runlocal           boot the iso thats already there"
	@echo "make repack             tar up initrd/ into the iso again, no compiling or internet"
	@echo "make clean              remove the kernels build output"
	@echo "make distclean          remove everything built or downloaded"
	@echo ""
	@echo "profiles: $(PROFILES) (default $(DEFAULT_PROFILE)), the lists are in config.mk"
	@$(foreach p,$(PROFILES),echo "  $(p): $(PROGRAMS_$(p))";)

.PHONY: $(PROFILES)
$(PROFILES):
	@:

.PHONY: FORCE
FORCE:
