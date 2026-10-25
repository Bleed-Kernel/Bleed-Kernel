DEFAULT_PROFILE ?= DEV

PROGRAMS_MINIMAL := verdict crime-scene about cabinet taskv tvi coreutils
PROGRAMS_DEV     := $(PROGRAMS_MINIMAL) doom tetris specseek uikit-demo exhibit playback tcc
PROGRAMS_RELEASE := $(PROGRAMS_DEV) quake2 fastfetch halflife hlds

USERPROG_SRC ?= ..

ifeq ($(origin JOBS),undefined)
JOBS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
endif

QEMU_NAME ?= The Bleed Kernel
MEMSZ     ?= 1G
USB       ?= ohci
LOG       ?=
# QEMU_AUDIODEV ?= coreaudio,out.buffer-count=10

DISK_SIZE_MB ?= 128
NVME_BIG_MB  ?= 1024
