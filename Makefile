IMAGE_NAME := bleed-kernel
OBJDIR := bin/obj
KERNEL_BIN := bin/bleed-kernel

CC := cc
LD := ld
OBJCOPY := objcopy

SYM_TOOL := tools/mksymtab
KERNEL_SYM := initrd/etc/kernel.sym

MEMSZ = 256M

CFLAGS := -g -O2 -Wall -Werror -Wextra -std=gnu11 \
          -nostdinc -ffreestanding -fno-stack-protector \
          -fno-stack-check -fno-lto -fno-PIC -fno-pie \
          -ffunction-sections -fdata-sections -fno-omit-frame-pointer \
          -m64 -march=x86-64 -mabi=sysv -mno-80387 -mno-mmx -mno-sse2 -mno-red-zone \
          -mcmodel=kernel -I kernel/include -I klibc/include \
          -MMD -MP

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --gc-sections \
           -T kernel.lds

KERNEL_C := $(shell find kernel -name '*.c')
KERNEL_S := $(shell find kernel -name '*.S')

KLIBC_C := $(shell find klibc -name '*.c')
KLIBC_S := $(shell find klibc -name '*.S')

KERNEL_OBJ := $(patsubst %.c,$(OBJDIR)/%.o,$(KERNEL_C)) \
              $(patsubst %.S,$(OBJDIR)/%.o,$(KERNEL_S))
KLIBC_OBJ := $(patsubst %.c,$(OBJDIR)/%.o,$(KLIBC_C)) \
             $(patsubst %.S,$(OBJDIR)/%.o,$(KLIBC_S))

OBJ := $(KERNEL_OBJ) $(KLIBC_OBJ)

DEPS := $(OBJ:.o=.d)
-include $(DEPS)

USER_PROGS := verdict meminfo specseek
USER_BASE := ../bleed-user

.PHONY: all
all: $(IMAGE_NAME).iso

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJ) -o $@

$(SYM_TOOL): tools/mksymtab.c
	$(CC) -O2 $< -o $@

$(KERNEL_SYM): $(KERNEL_BIN) $(SYM_TOOL)
	@mkdir -p $(dir $@)
	nm -n --defined-only $(KERNEL_BIN) > bin/kernel.sym.txt
	$(SYM_TOOL) bin/kernel.sym.txt $@

limine/limine:
	rm -rf limine
	git clone https://codeberg.org/Limine/Limine.git limine --branch=v10.x-binary --depth=1
	$(MAKE) -C limine

.PHONY: userprogs
userprogs:
	@for p in $(USER_PROGS); do \
		dir=$(USER_BASE)/$$p; \
		test -d $$dir; \
		$(MAKE) -C $$dir; \
	done

.PHONY: install-userprogs
install-userprogs: userprogs
	@mkdir -p initrd/bin
	@for p in $(USER_PROGS); do \
		cp $(USER_BASE)/$$p/bin/$$p initrd/bin/$$p; \
	done

.PHONY: initrd
initrd: $(KERNEL_SYM) install-userprogs
	tar -cf initrd/initrd.tar \
		initrd/fonts/* initrd/bin/* initrd/etc/*

$(IMAGE_NAME).iso: limine/limine $(KERNEL_BIN) initrd
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v $(KERNEL_BIN) iso_root/boot/
	cp -v wallpaper.png iso_root/
	mkdir -p iso_root/boot/limine
	cp -v limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	cp -v initrd/initrd.tar iso_root/boot/
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

.PHONY: run
run: $(IMAGE_NAME).iso
	qemu-system-x86_64 -cdrom $(IMAGE_NAME).iso --enable-kvm -cpu host -boot d -m $(MEMSZ) -serial stdio

.PHONY: clean
clean:
	rm -rf bin $(IMAGE_NAME).iso iso_root
	rm -f bin/kernel.sym.txt initrd/etc/kernel.sym
	rm -f tools/mksymtab
	find kernel klibc -name '*.o' -delete
	find kernel klibc -name '*.d' -delete
	find initrd -name '*.tar' -delete
	@for p in $(USER_PROGS); do \
		$(MAKE) -C $(USER_BASE)/$$p clean || true; \
	done
