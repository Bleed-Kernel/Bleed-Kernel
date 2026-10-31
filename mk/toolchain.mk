include $(dir $(lastword $(MAKEFILE_LIST)))tools.mk

KERNEL_CODEGEN := -g -O2 -Wall -Werror -Wextra -std=gnu11 \
                  -nostdinc -ffreestanding -fno-stack-protector \
                  -fno-stack-check -fno-lto -fno-PIC -fno-pie \
                  -ffunction-sections -fdata-sections -fno-omit-frame-pointer \
                  -m64 -march=x86-64 -mabi=sysv -mcmodel=kernel \
                  -mno-red-zone \
                  -msoft-float \
                  -mno-80387 \
                  -mno-mmx \
                  -mno-sse \
                  -mno-sse2 \
                  -mno-sse3 \
                  -mno-ssse3 \
                  -mno-sse4 \
                  -mno-sse4.1 \
                  -mno-sse4.2 \
                  -mno-avx \
                  -mno-avx2 \
                  -mno-avx512f

CFLAGS := $(KERNEL_CODEGEN) -I kernel/include -I klibc/include -MMD -MP

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
           -T kernel.lds
