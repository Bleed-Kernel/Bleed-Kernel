OVMF_FW := edk2-ovmf/OVMF-pure-efi.fd

# homebrew qemu has no sdl, so only use it on mac if the qemu on PATH was built with it
QEMU_MAC_DISPLAY := $(if $(shell qemu-system-x86_64 -display help 2>/dev/null | grep -x sdl),sdl,cocoa)

# first backend this qemu was built with, from what each os would want most
QEMU_AUDIO_DRIVERS = $(shell qemu-system-x86_64 -audiodev help 2>/dev/null)
qemu_audio_pick    = $(or $(firstword $(foreach d,$(1),$(filter $(d),$(QEMU_AUDIO_DRIVERS)))),none)

ifeq ($(UNAME_S),Darwin)
# coreaudio only keeps 4 buffers of ~11ms by default, less than qemu stalls for when the
# window is busy (moving the mouse)
QEMU_AUDIODEV ?= coreaudio,out.buffer-count=10
else ifneq ($(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),)
QEMU_AUDIODEV ?= $(call qemu_audio_pick,dsound sdl)
else
QEMU_AUDIODEV ?= $(call qemu_audio_pick,pipewire pa alsa sdl oss)
endif
QEMU_AUDIO := -audiodev $(QEMU_AUDIODEV),id=snd0 -device AC97,audiodev=snd0

QEMU_NET ?= -nic user,model=e1000,hostfwd=udp::27015-:27015

#apple silicon emulation sucks, if you are getting performance issues, please test it on an x86_64 machine before submitting an issue!
#SMAP and SMEP are known to be funny on mac, it may not emulate it at all, just be careful but bleed should handle it.
QEMU_HOST_MAC := -accel tcg,thread=multi,tb-size=1024 -cpu max -display $(QEMU_MAC_DISPLAY)

ifeq ($(UNAME_S)$(UNAME_M),Darwinarm64)
QEMU_HOST := $(QEMU_HOST_MAC)
else
QEMU_HOST := --enable-kvm -display sdl
endif

ifeq ($(USB),xhci)
QEMU_USB_HC := -device qemu-xhci,id=usb
else ifeq ($(USB),ohci)
QEMU_USB_HC := -device pci-ohci,id=usb
else
$(error USB=$(USB) isnt a thing, use USB=ohci or USB=xhci)
endif
QEMU_USB := $(QEMU_USB_HC) -device usb-kbd,bus=usb.0 -device usb-mouse,bus=usb.0 \
            -drive file=$(USB_DISK),format=raw,if=none,id=usbstick \
            -device usb-storage,bus=usb.0,drive=usbstick

# asking for a controller by name means you want to test it, so no PS/2 to fall back on
ifneq ($(filter command line environment,$(origin USB)),)
QEMU_USB += -machine pc,i8042=off
endif

# what an intel mac looks like to us: no PS/2 controller at all, the keyboard and trackpad hang off the usb root ports
QEMU_OHCI_INPUT := -machine pc,i8042=off
QEMU_UEFI       := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FW)

LOG_SOCK ?= /tmp/bleed-com3-$(shell id -u).sock
QEMU_SERIAL_SETUP = rm -f "$(LOG_SOCK)"; serial="-serial mon:stdio"; \
	if [ "$(LOG)" != shared ] && sh tools/logterm.sh "$(LOG_SOCK)"; then \
		serial="$$serial -serial null -serial unix:$(LOG_SOCK),server=on,wait=on"; \
	fi;

# $(1) is extra qemu args for the run-* variants
define run_qemu
	$(QEMU_SERIAL_SETUP) qemu-system-x86_64 \
		-name "$(QEMU_NAME)" \
		-cdrom $(ISO) \
		$(QEMU_HOST) \
		-boot d \
		-m $(MEMSZ) \
		$$serial \
		-smp 4 \
		-drive format=raw,file=$(IDE_DISK) \
		-device ich9-ahci,id=ahci \
		-drive file=$(SATA_DISK),format=raw,if=none,id=sata0 \
		-device ide-hd,drive=sata0,bus=ahci.0 \
		-drive file=$(NVME_DISK),format=raw,if=none,id=nvm0 \
		-device nvme,serial=bleed-nvme-1,drive=nvm0 \
		$(QEMU_AUDIO) \
		$(QEMU_NET) \
		$(QEMU_USB) \
		$(1)
endef
