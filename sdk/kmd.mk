ifeq ($(KMD_NAME),)
$(error set KMD_NAME before including kmd.mk)
endif

SDK := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
include $(SDK)/sdk.mk

KMD_OUT   := bin/$(KMD_NAME).KMD
KMD_SYMS  := bin/$(KMD_NAME).syms
KMD_INFO  := bin/obj/kmd_info.c
KMD_HAVE  := bin/obj/available.syms
KMD_OBJS  := $(patsubst src/%.c,bin/obj/%.o,$(shell find src -name '*.c')) bin/obj/kmd_info.o
DEP_SYMS  := $(foreach d,$(KMD_DEPENDS),$(SDK)/symbols/$(d).syms)

KMD_CFLAGS := $(KERNEL_CODEGEN) -fno-common -Iinclude -Isrc -I$(SDK)/include -MMD -MP

.PHONY: all clean distclean
all: $(KMD_OUT) $(KMD_SYMS)

bin/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC] $(KMD_NAME) $<"
	@$(CC) $(KMD_CFLAGS) -c $< -o $@

# what the kernel reads before it links anything, what to load first and which sdk this came from
$(KMD_INFO): Makefile $(SDK)/VERSION
	@mkdir -p $(dir $@)
	@printf '#include <kmd/kmd.h>\n' > $@
	@printf 'KMD_DEPENDS("%s");\n' "$(strip $(KMD_DEPENDS))" >> $@
	@printf '__attribute__((section(".kmd_kernel"), used)) static const char kmd_kernel[] = "%s";\n' \
		"$$(cat $(SDK)/VERSION)" >> $@

bin/obj/kmd_info.o: $(KMD_INFO)
	@$(CC) $(KMD_CFLAGS) -c $< -o $@

$(KMD_HAVE): $(SDK)/symbols/kernel.syms $(DEP_SYMS) Makefile
	@for f in $(DEP_SYMS); do \
		[ -f "$$f" ] || { echo "ERROR: the sdk has nothing for $$(basename $$f .syms), is that driver in it?"; exit 1; }; \
	done
	@cat $(SDK)/symbols/kernel.syms $(DEP_SYMS) | sort -u > $@

# the kernel would refuse it at boot anyway, better to hear it now
$(KMD_OUT): $(KMD_OBJS) $(KMD_HAVE)
	@echo "[KMD] $@"
	@$(LD) -r --strip-debug $(KMD_OBJS) -o $@.tmp
	@missing=$$($(NM) -u $@.tmp | awk '$$1 == "U" {print $$2}' | sort -u | comm -23 - $(KMD_HAVE)); \
	if [ -n "$$missing" ]; then \
		echo "ERROR: $(KMD_NAME) uses things neither the kernel nor $(if $(KMD_DEPENDS),$(KMD_DEPENDS),any driver) have:"; \
		for s in $$missing; do echo "    $$s"; done; \
		rm -f $@.tmp; exit 1; \
	fi
	@mv $@.tmp $@

$(KMD_SYMS): $(KMD_OUT)
	@$(NM) -g --defined-only $< | awk '{print $$3}' | sort -u > $@

clean:
	rm -rf bin

distclean: clean

-include $(KMD_OBJS:.o=.d)
