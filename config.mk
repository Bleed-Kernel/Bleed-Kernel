DEFAULT_PROFILE ?= DEV

PROGRAMS_MINIMAL := verdict crime-scene about cabinet taskv tvi coreutils
PROGRAMS_DEV     := $(PROGRAMS_MINIMAL) doom tetris specseek uikit-demo exhibit playback tcc halflife hlds
PROGRAMS_RELEASE := $(PROGRAMS_DEV) quake2 fastfetch

USERPROG_SRC ?= ..

DRIVERS := HID USB USB-HID USB-Storage XHCI EHCI OHCI VirtIO VirtIO-Input DW-I2C I2C-HID \
           IDE AHCI NVMe AC97 E1000 CrosEC

ifeq ($(origin JOBS),undefined)
JOBS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
endif

QEMU_NAME ?= The Bleed Kernel
MEMSZ     ?= 1G
USB       ?= ohci
LOG       ?=
PACKAGE   ?=
# QEMU_AUDIODEV ?= coreaudio,out.buffer-count=10

DISK_SIZE_MB ?= 128
DATA_SPARE_MB ?= 64
