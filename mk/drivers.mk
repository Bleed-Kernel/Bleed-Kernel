DRIVER_MODULES := kernel/src/drivers
DRIVER_SRC     ?= $(DRIVER_MODULES)
KMD_OUT    := initrd/drivers
KMD_CONF   := initrd/etc/drivers.conf

KMD_MAKE = $(MAKE) -s -C "$(DRIVER_SRC)/KMD-$$d" BLEED_SDK=$(abspath $(SDK_DIR))

.PHONY: drivers-fetch
drivers-fetch:
	@for d in $(DRIVERS); do \
		[ -f "$(DRIVER_SRC)/KMD-$$d/Makefile" ] && continue; \
		if [ "$(DRIVER_SRC)" = $(DRIVER_MODULES) ]; then \
			echo "[KMD] Fetching submodule $(DRIVER_MODULES)/KMD-$$d"; \
			git submodule update --init "$(DRIVER_MODULES)/KMD-$$d" || exit 1; \
		else \
			echo "ERROR: $(DRIVER_SRC)/KMD-$$d not found"; \
			exit 1; \
		fi; \
	done

.PHONY: drivers
drivers: $(KERNEL_EXPORTS) sdk-tree
	@mkdir -p $(KMD_OUT) $(dir $(KMD_CONF))
	@for d in $(DRIVERS); do \
		echo "[KMD] $$d"; \
		$(KMD_MAKE) || exit 1; \
		COPYFILE_DISABLE=1 cp "$(DRIVER_SRC)/KMD-$$d/bin/$$d.KMD" $(KMD_OUT)/; \
		cp -p "$(DRIVER_SRC)/KMD-$$d/bin/$$d.syms" $(SDK_DIR)/symbols/; \
	done
	@for f in $(KMD_OUT)/*.KMD; do \
		[ -e "$$f" ] || continue; \
		n=$${f##*/}; n=$${n%.KMD}; \
		case " $(DRIVERS) " in *" $$n "*) ;; *) echo "[KMD] Removing $$n, its not in DRIVERS"; rm -f "$$f" ;; esac; \
	done
	@echo "# made by the build from DRIVERS in config.mk, they load top to bottom" > $(KMD_CONF)
	@for d in $(DRIVERS); do echo $$d >> $(KMD_CONF); done

.PHONY: drivers-clean
drivers-clean:
	@for d in $(DRIVERS); do \
		rm -rf "$(DRIVER_SRC)/KMD-$$d/bin"; \
	done
	rm -rf $(KMD_OUT) $(KMD_CONF)
