include ../../Makefile.include RUSTUP-VERSION ../../download-files.include
DEPS = ../../Makefile.include Makefile RUSTUP-VERSION ../../download-files.include

PREFIX=$(NATIVEPREFIX)
PLATFORM=$(NATIVEPLATFORM)

export RUSTUP_HOME=$(PREFIX)/.rustup
export CARGO_HOME=$(PREFIX)/.cargo

APP=$(PLATFORM)/bin/$(APPNAME)

RUSTUP_ENV_VARS = RUSTUP_HOME=$(PREFIX)/.rustup \
	CARGO_HOME=$(PREFIX)/.cargo
RUSTUP = $(RUSTUP_ENV_VARS) $(PREFIX)/bin/rustup

CLEANUP_CMD=[ -e $(PREFIX)/bin/rustup ] \
	&& $(RUSTUP) self uninstall -y \
	&& rm -f $(PREFIX)/bin/rustup \
	&& rm -f $(PREFIX)/bin/cargo || true

all: .installed-$(PLATFORM)

$(PLATFORM): $(DEPS) | $(TARBALLS_LOCATION)/$(ARCHIVE).$(HASH_TYPE)
	rm -rf $(PLATFORM)/*; mkdir -p $(PLATFORM)
	cd $(PLATFORM); $(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS) $(TARBALLS_LOCATION)/$(ARCHIVE)

$(APP): $(PLATFORM)
	bash $(PLATFORM)/rustup-init.sh --no-modify-path -y

.installed-$(PLATFORM): $(APP)
	ln -sf $(CARGO_HOME)/bin/rustup $(PREFIX)/bin/rustup
	ln -sf $(CARGO_HOME)/bin/cargo $(PREFIX)/bin/cargo

	$(RUSTUP) default stable

ifeq ($(CROSS_COMPILING),yes)
	$(RUSTUP) target add $(HOST)
endif

	touch $@

clean:
	$(CLEANUP_CMD)
	rm -f .installed-$(PLATFORM)

distclean::
	$(CLEANUP_CMD)
	rm -rf $(PLATFORM) .installed-$(PLATFORM)
