UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S)$(UNAME_M),Darwinarm64)
CC := x86_64-elf-gcc
LD := x86_64-elf-ld
NM := x86_64-elf-nm
AR := x86_64-elf-ar
AS := x86_64-elf-as
USERPROG_TOOLCHAIN := CC=x86_64-elf-gcc AR=x86_64-elf-ar AS=x86_64-elf-as
else
CC := cc
LD := ld
NM := nm
USERPROG_TOOLCHAIN :=
endif
