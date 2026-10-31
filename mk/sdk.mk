SDK_DIR := bin/bleed-sdk
SDK_TAR := bin/bleed-sdk-$(BUILD_VERSION).tar.gz

# headers are copied keeping their times or every driver rebuilds from scratch on every build
.PHONY: sdk-tree
sdk-tree: $(KERNEL_NM) $(PROC_VERSION_FILE) drivers-fetch
	@echo "[SDK] $(SDK_DIR)"
	@rm -rf $(SDK_DIR)/include
	@mkdir -p $(SDK_DIR)/include $(SDK_DIR)/symbols
	@cp -Rp kernel/include/. klibc/include/. $(SDK_DIR)/include/
	@for d in $(DRIVERS); do \
		if [ -d "$(DRIVER_SRC)/KMD-$$d/include" ]; then cp -Rp "$(DRIVER_SRC)/KMD-$$d/include/." $(SDK_DIR)/include/; fi; \
	done
	@cp -p sdk/kmd.mk mk/tools.mk $(SDK_DIR)/
	@if [ -f sdk/README.md ]; then cp -p sdk/README.md $(SDK_DIR)/; fi
	@printf '# made by the kernel build, dont edit\ninclude $$(dir $$(lastword $$(MAKEFILE_LIST)))tools.mk\n\nKERNEL_CODEGEN := %s\n' \
		"$(strip $(KERNEL_CODEGEN))" > $(SDK_DIR)/sdk.mk
	@if ! cmp -s $(PROC_VERSION_FILE) $(SDK_DIR)/VERSION; then cp $(PROC_VERSION_FILE) $(SDK_DIR)/VERSION; fi
	@if [ "$(DRIVER_SRC)" = $(DRIVER_MODULES) ]; then ln -sfn $(abspath $(SDK_DIR)) $(DRIVER_MODULES)/bleed-sdk; fi
	@awk '$$2 ~ /^[A-Z]$$/ && $$2 != "U" {print $$3}' $(KERNEL_NM) | sort -u > $(SDK_DIR)/symbols/kernel.syms.tmp
	@if cmp -s $(SDK_DIR)/symbols/kernel.syms.tmp $(SDK_DIR)/symbols/kernel.syms; then \
		rm $(SDK_DIR)/symbols/kernel.syms.tmp; \
	else \
		mv $(SDK_DIR)/symbols/kernel.syms.tmp $(SDK_DIR)/symbols/kernel.syms; \
	fi
.PHONY: sdk
sdk: drivers
	@echo "[SDK] $(SDK_TAR)"
	@COPYFILE_DISABLE=1 tar -czf $(SDK_TAR) -C $(dir $(SDK_DIR)) $(notdir $(SDK_DIR))
