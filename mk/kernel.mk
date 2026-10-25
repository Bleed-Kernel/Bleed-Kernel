OBJDIR     := bin/obj
KERNEL_BIN := bin/bleed-kernel

PROC_VERSION_FILE := initrd/proc/version
BUILD_GIT_HASH    := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
BUILD_GIT_COUNT   := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
BUILD_GIT_DIRTY   := $(shell if git diff --quiet --ignore-submodules HEAD >/dev/null 2>&1; then echo ""; else echo "-dirty"; fi)
BUILD_VERSION     := r$(BUILD_GIT_COUNT)-g$(BUILD_GIT_HASH)$(BUILD_GIT_DIRTY)

KERNEL_OBJ := $(patsubst %.c, $(OBJDIR)/%.o, $(shell find kernel -name '*.c')) \
              $(patsubst %.S, $(OBJDIR)/%.o, $(shell find kernel -name '*.S'))

KLIBC_OBJ := $(patsubst %.c, $(OBJDIR)/%.o, $(shell find klibc -name '*.c')) \
             $(patsubst %.S, $(OBJDIR)/%.o, $(shell find klibc -name '*.S'))

OBJ  := $(KERNEL_OBJ) $(KLIBC_OBJ)
DEPS := $(OBJ:.o=.d)
-include $(DEPS)

MK_SYMTAB  := bin/tools/mksymtab
KERNEL_NM  := bin/kernel.nm
KERNEL_SYM := initrd/etc/kernel.sym

$(PROC_VERSION_FILE): FORCE
	@mkdir -p $(dir $@)
	@tmp="$@.tmp"; \
	printf "%s\n" "$(BUILD_VERSION)" > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$@" "$$tmp"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
	fi

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "[AS] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJ)
	@mkdir -p $(dir $@)
	@echo "[LD] $@"
	@$(LD) $(LDFLAGS) $(OBJ) -o $@

$(MK_SYMTAB): tools/mksymtab.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@cc -O2 -Wall -Wextra -std=gnu11 $< -o $@

$(KERNEL_NM): $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	@$(NM) -n $(KERNEL_BIN) > $@

$(KERNEL_SYM): $(KERNEL_NM) $(MK_SYMTAB)
	@mkdir -p $(dir $@)
	@$(MK_SYMTAB) $(KERNEL_NM) $@
