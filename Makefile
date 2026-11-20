OO_PS4_TOOLCHAIN ?= $(abspath ../toolchain/OpenOrbis/PS4Toolchain)
HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
OO_HOST ?= macos
LLVM ?= /opt/homebrew/opt/llvm/bin
HOST_CXX ?= /usr/bin/clang++
HOST_CC ?= /usr/bin/clang
PS4_LD ?= /opt/homebrew/opt/lld/bin/ld.lld
CREATE_FSELF ?= $(OO_PS4_TOOLCHAIN)/bin/macos/create-fself-macos
else
OO_HOST ?= linux
LLVM ?= /usr/bin
HOST_CXX ?= $(LLVM)/clang++
HOST_CC ?= $(LLVM)/clang
PS4_LD ?= $(LLVM)/ld.lld
CREATE_FSELF ?= $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself
endif
CC := $(LLVM)/clang++
LD := $(PS4_LD)
PKGTOOL ?= $(OO_PS4_TOOLCHAIN)/bin/$(OO_HOST)/PkgTool.Core
export PKGTOOL
$(shell mkdir -p build)

CONTENT_ID := IV0000-HBST00001_00-HBSTORE000000000
# Installer permissions used by flatz/ps4_remote_pkg_installer (Makefile AUTH_INFO).
INSTALLER_AUTH_INFO := $(shell cat tools/installer-auth.hex)
CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -mstackrealign -MMD -MP -D_POSIX_C_SOURCE=200809L -O2 -std=c++11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers -isysroot $(OO_PS4_TOOLCHAIN) -isystem $(OO_PS4_TOOLCHAIN)/include -isystem $(OO_PS4_TOOLCHAIN)/include/c++/v1
CFLAGS += -ffile-prefix-map=$(OO_PS4_TOOLCHAIN)=/openorbis
LIBS := -lc++ -lc++abi -lc -lm -lpthread -lkernel -lSceVideoOut -lScePad -lSceUserService -lSceSystemService -lSceSysmodule -lSceNet -lSceNetCtl -lSceSsl -lSceHttp -lSceBgft -lSceLncUtil

.PHONY: all preview test validate icons clean
all: build/$(CONTENT_ID).pkg

icons: assets/brand/nullstore-bag.png tools/build_icons.py
	python3 tools/build_icons.py
assets/brand/store-icon.png sce_sys/icon0.png: icons
	@test -f $@

build/trusted_roots.h: tools/embed_trust.py assets/service/cacert.pem
	python3 tools/embed_trust.py

build/main.o: src/ps4.cpp src/ui.h src/catalog.h src/installer.h src/network_ps4.h src/startup_ps4.h src/install_log.h src/text_input_ps4.h src/data_root.h
	$(CC) $(CFLAGS) -c $< -o $@
build/network_ps4.o: src/network_ps4.cpp src/network_ps4.h src/catalog.h src/data_root.h build/trusted_roots.h
	$(CC) $(CFLAGS) -c $< -o $@
build/installer.o: src/installer.cpp src/installer.h src/catalog.h src/queue_store.h src/bgft_abi.h src/install_log.h src/path_info.h
	$(CC) $(CFLAGS) -c $< -o $@
build/installer_ps4.o: src/installer_ps4.cpp src/installer.h src/bgft_abi.h src/network_ps4.h src/install_log.h
	$(CC) $(CFLAGS) -c $< -o $@
build/text_input_ps4.o: src/text_input_ps4.cpp src/text_input_ps4.h src/network_ps4.h
	$(CC) $(CFLAGS) -c $< -o $@

build/hbstore.elf: tools/link_ps4.py build/main.o build/network_ps4.o build/installer.o build/installer_ps4.o build/text_input_ps4.o
	python3 tools/link_ps4.py $(LD) --no-mmap-output-file -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x --eh-frame-hdr -L$(OO_PS4_TOOLCHAIN)/lib $(OO_PS4_TOOLCHAIN)/lib/crt1.o $(filter %.o,$^) $(LIBS) -lSceImeDialog -lSceCommonDialog -o $@

eboot.bin: build/hbstore.elf tools/installer-auth.hex Makefile
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(CREATE_FSELF) -in=$< -out=build/hbstore.oelf --eboot $@ --ptype fake --paid 0x3100000000000002 --authinfo $(INSTALLER_AUTH_INFO)

sce_sys/param.sfo: tools/package.py
	python3 tools/package.py sfo

pkg.gp4: icons eboot.bin sce_sys/param.sfo tools/package.py assets/font.ttf assets/server.txt assets/service/cacert.pem assets/service/TLS-LICENSE.txt assets/service/TLS-PROVENANCE.txt sce_sys/icon0.png
	python3 tools/package.py gp4

build/$(CONTENT_ID).pkg: pkg.gp4
	python3 tools/package.py build $(PKGTOOL)

validate: build/$(CONTENT_ID).pkg
	$(PKGTOOL) pkg_validate $<
	python3 tools/verify_package.py $< --tool $(PKGTOOL)

preview: build/preview
	./build/preview
build/preview: src/preview.cpp src/ui.h src/catalog.h src/installer.h
	$(HOST_CXX) -DPORTAL_HOST -std=c++11 -O2 -Wno-unused-function -idirafter $(OO_PS4_TOOLCHAIN)/include src/preview.cpp -o $@

# Host tests
build/test_catalog: tests/test_catalog.cpp src/ui.h src/catalog.h src/installer.h
	$(HOST_CXX) -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@
build/test_network_ps4: tests/test_network_ps4.cpp src/network_ps4.cpp src/network_ps4.h build/trusted_roots.h
	$(HOST_CXX) -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -lpthread -o $@
build/test_file_info: tests/test_file_info.c src/path_info.h
	$(HOST_CC) -std=c11 -O1 -g -fsanitize=address,undefined $< -o $@
build/test_installer: tests/test_installer.cpp src/installer.cpp src/installer.h src/queue_store.h src/path_info.h src/install_log.h src/bgft_abi.h
	$(HOST_CXX) -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/test_installer.cpp src/installer.cpp -lpthread -o $@
build/test_installer_ps4: tests/test_installer_ps4.cpp src/installer_ps4.cpp src/installer.h src/bgft_abi.h src/install_log.h
	$(HOST_CXX) -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@
build/test_installer_progress: tests/test_installer_progress.cpp src/installer.cpp src/installer.h src/queue_store.h
	$(HOST_CXX) -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/test_installer_progress.cpp src/installer.cpp -lpthread -o $@
build/test_installer_retry: tests/test_installer_retry.cpp src/installer.cpp src/installer.h src/queue_store.h
	$(HOST_CXX) -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/test_installer_retry.cpp src/installer.cpp -lpthread -o $@
build/test_startup_ps4: tests/test_startup_ps4.cpp src/startup_ps4.h src/install_log.h
	$(HOST_CXX) -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@
build/test_text_input: tests/test_text_input.cpp src/text_input_ps4.cpp src/text_input_ps4.h
	$(HOST_CXX) -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@

test: build/test_catalog build/test_network_ps4 build/test_file_info build/test_installer build/test_installer_ps4 build/test_installer_progress build/test_installer_retry build/test_startup_ps4 build/test_text_input
	python3 -m unittest discover -s tests -v
	./build/test_catalog
	./build/test_network_ps4
	./build/test_file_info
	./build/test_installer
	./build/test_installer_ps4
	./build/test_installer_progress
	./build/test_installer_retry
	./build/test_startup_ps4
	./build/test_text_input

clean:
	rm -rf build eboot.bin pkg.gp4 sce_sys/param.sfo

-include $(wildcard build/*.d)
