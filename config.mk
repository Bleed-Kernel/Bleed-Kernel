DEFAULT_PROFILE ?= DEV

PROGRAMS_MINIMAL := verdict crime-scene about cabinet taskv tvi coreutils bpm
PROGRAMS_DEV     := $(PROGRAMS_MINIMAL) tetris specseek exhibit playback tcc curl
PROGRAMS_RELEASE := $(PROGRAMS_DEV)

USERPROG_SRC ?= ..

DRIVERS := HID USB USB-HID USB-Storage XHCI EHCI OHCI VirtIO VirtIO-Input DW-I2C I2C-HID \
           IDE AHCI NVMe EMMC HDA SOF AC97 E1000 CrosEC

ifeq ($(origin JOBS),undefined)
JOBS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
endif

QEMU_NAME ?= The Bleed Kernel
# packages built from their repos (mk/programs.mk) into .packages/, the nvme disk carries them at
# /mnt/nvme/bpkg for bpm install. BPKGS= for none. firefox is only packed if Bleed-Firefox has a
# built one (make firefox-build firefox-package over there), its never built from here
BPKGS     ?= doom quake2 halflife firefox fastfetch
# firefox wont start in 1G
MEMSZ     ?= $(if $(filter firefox,$(BPKGS)),4G,1G)
USB       ?= ohci
# USB_FS=ext4 (or ext3) formats the usb stick with that instead of ext2, its own usbdisk-ext4.img
# so the ext2 one is still there when u go back
USB_FS    ?= ext2
# EMMC=1 gives make run an emmc on an sd host controller (emmcdisk.img at /mnt/emmc), boot with
# emmc=rw on the kernel command line to write to it
EMMC      ?=
AUDIO     ?= ac97
LOG       ?=
PACKAGE   ?=
# QEMU_AUDIODEV ?= coreaudio,out.buffer-count=10

DISK_SIZE_MB ?= 128
DATA_SPARE_MB ?= 64
