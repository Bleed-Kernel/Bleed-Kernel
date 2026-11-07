USER_BIN_DIR := external
INITRD_BIN   := initrd/bin

USERPROGS_SH = MAKE="$(MAKE)" USERPROG_SRC="$(USERPROG_SRC)" USER_BIN_DIR="$(USER_BIN_DIR)" \
               INITRD="$(abspath initrd)" USERPROG_TOOLCHAIN="$(USERPROG_TOOLCHAIN)" SHARED_LIBS="$(SHARED_LIBS)" \
               sh tools/userprogs.sh

# clones/pulls/builds whatever the profile wants and leaves only those in initrd/bin
.PHONY: userprogs
userprogs:
	@echo "[USER] $(PROFILE): $(PROGRAMS)"
	@$(USERPROGS_SH) build $(call program_entries,$(PROGRAMS))
