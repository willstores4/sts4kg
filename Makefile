OO_PS4_TOOLCHAIN ?= $(abspath ../toolchain/OpenOrbis/PS4Toolchain)
# Host detection: macOS (original setup) or Linux (GitHub Actions / CI).
HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
OO_HOST ?= macos
LLVM ?= /opt/homebrew/opt/llvm/bin
PS4_LD ?= /opt/homebrew/opt/lld/bin/ld.lld
CREATE_FSELF ?= $(OO_PS4_TOOLCHAIN)/bin/macos/create-fself-macos
else
OO_HOST ?= linux
LLVM ?= /usr/bin
PS4_LD ?= $(LLVM)/ld.lld
CREATE_FSELF ?= $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself
endif
CC := $(LLVM)/clang++
LD := $(PS4_LD)
PKGTOOL ?= $(OO_PS4_TOOLCHAIN)/bin/$(OO_HOST)/PkgTool.Core
export PKGTOOL
CONTENT_ID := IV0000-NULL00001_00-NULLSTOR00000000
# Installer permissions used by flatz/ps4_remote_pkg_installer (Makefile AUTH_INFO).
INSTALLER_AUTH_INFO := $(shell cat tools/installer-auth.hex)
CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -mstackrealign -MMD -MP -D_POSIX_C_SOURCE=200809L -O2 -std=c++11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers -isysroot $(OO_PS4_TOOLCHAIN) -isystem $(OO_PS4_TOOLCHAIN)/include -isystem $(OO_PS4_TOOLCHAIN)/include/c++/v1
CFLAGS += -ffile-prefix-map=$(OO_PS4_TOOLCHAIN)=/openorbis
LIBS := -lc++ -lc++abi -lc -lm -lpthread -lkernel -lSceVideoOut -lScePad -lSceUserService -lSceSystemService -lSceSysmodule -lSceNet -lSceNetCtl -lSceSsl -lSceHttp -lSceBgft -lSceLncUtil
.PHONY: all snapshot preview test validate icons
all: build/$(CONTENT_ID).pkg
icons: assets/brand/nullstore-bag.png tools/build_icons.py
	python3 tools/build_icons.py
assets/brand/store-icon.png assets/brand/service-icon.png sce_sys/icon0.png: icons
	@test -f $@
build/main.o: src/ps4.cpp src/ui.h src/catalog.h src/download_controller.h src/service_client.h src/download_model.h src/providers.h src/installer.h src/network_ps4.h src/download_paths.h src/startup_ps4.h src/install_log.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/download_manager.o: src/download_manager.cpp src/download_manager.h src/download_model.h src/providers.h src/catalog.h src/torrent.h src/queue_store.h src/network_ps4.h src/download_paths.h src/path_info.h
	$(CC) $(CFLAGS) -c $< -o $@
build/torrent.o: src/torrent.c src/torrent.h src/torrent_resume.h src/torrent_checkpoint.h
	$(LLVM)/clang $(filter-out -std=c++11,$(CFLAGS)) -Wno-misleading-indentation -c $< -o $@
build/installer.o: src/installer.cpp src/installer.h src/queue_store.h src/bgft_abi.h src/download_paths.h
	$(CC) $(CFLAGS) -c $< -o $@
build/installer_ps4.o: src/installer_ps4.cpp src/installer.h src/bgft_abi.h
	$(CC) $(CFLAGS) -c $< -o $@
build/trusted_roots.h: tools/embed_trust.py assets/service/cacert.pem
	python3 tools/embed_trust.py
pkg.gp4: assets/service/cacert.pem assets/service/TLS-LICENSE.txt assets/service/TLS-PROVENANCE.txt
build/IV0000-NULL00002_00-NULLSTORSERVICE0.pkg: assets/service/cacert.pem assets/service/TLS-LICENSE.txt assets/service/TLS-PROVENANCE.txt
build/network_ps4.o: src/network_ps4.cpp src/network_ps4.h src/catalog.h src/service_log.h build/trusted_roots.h
	$(CC) $(CFLAGS) -c $< -o $@
build/service_protocol.o: src/service_protocol.cpp src/service_protocol.h
	$(CC) $(CFLAGS) -c $< -o $@
build/service_client.o: src/service_client.cpp src/service_client.h src/service_protocol.h src/service_start_ps4.h
	$(CC) $(CFLAGS) -c $< -o $@
build/service_start_ps4.o: src/service_start_ps4.cpp src/service_start_ps4.h src/queue_store.h src/service_files.h src/service_setup_log.h src/path_info.h
	$(CC) $(CFLAGS) -c $< -o $@
build/service_server.o: src/service_server.cpp src/service_server.h src/service_protocol.h src/download_manager.h src/service_log.h
	$(CC) $(CFLAGS) -DPORTAL_SERVICE_DAEMON -c $< -o $@
build/service_main.o: src/service_main.cpp src/service_server.h src/service_start_ps4.h src/network_ps4.h src/service_log.h
	$(CC) $(CFLAGS) -DPORTAL_SERVICE_DAEMON -c $< -o $@
build/network_service.o: src/network_ps4.cpp src/network_ps4.h src/catalog.h src/service_log.h build/trusted_roots.h
	$(CC) $(CFLAGS) -DPORTAL_SERVICE_DAEMON -c $< -o $@
build/test_network_ps4: tests/test_network_ps4.cpp src/network_ps4.cpp src/network_ps4.h build/trusted_roots.h
	/usr/bin/clang++ -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -lpthread -o $@
.PHONY: test-network
test: test-network
test-network: build/test_network_ps4
	./build/test_network_ps4
build/service-log.o: src/service_log.cpp src/service_log.h
	$(CC) $(CFLAGS) -DPORTAL_SERVICE_DAEMON -c $< -o $@
build/portal-service.elf: tools/link_ps4.py build/service_main.o build/service_server.o build/service_protocol.o build/service_start_ps4.o build/network_service.o build/download_manager.o build/torrent.o build/service-log.o
	python3 tools/link_ps4.py $(LD) --no-mmap-output-file -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x --eh-frame-hdr -L$(OO_PS4_TOOLCHAIN)/lib $(OO_PS4_TOOLCHAIN)/lib/crt1.o $(filter %.o,$^) $(LIBS) -o $@
assets/service/eboot.bin: build/portal-service.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(CREATE_FSELF) -in=$< -out=build/portal-service.oelf --eboot $@ -ptype fake --paid 0x3100000000000002
assets/service/daemon.sfo: tools/service_bundle.py
	python3 tools/service_bundle.py
build/download_controller.o: src/download_controller.cpp src/download_controller.h src/service_client.h src/download_manager.h src/catalog.h src/network_ps4.h
	$(CC) $(CFLAGS) -c $< -o $@
build/nullstore.elf: build/text_input_ps4.o tools/link_ps4.py build/download_controller.o build/main.o build/download_manager.o build/torrent.o build/service_client.o build/service_protocol.o build/service_start_ps4.o build/network_ps4.o build/installer.o build/installer_ps4.o
	python3 tools/link_ps4.py $(LD) --no-mmap-output-file -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x --eh-frame-hdr -L$(OO_PS4_TOOLCHAIN)/lib $(OO_PS4_TOOLCHAIN)/lib/crt1.o $(filter %.o,$^) $(LIBS) -lSceImeDialog -lSceCommonDialog -o $@
eboot.bin: build/nullstore.elf tools/installer-auth.hex Makefile
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(CREATE_FSELF) -in=$< -out=build/nullstore.oelf --eboot $@ --ptype fake --paid 0x3100000000000002 --authinfo $(INSTALLER_AUTH_INFO)
sce_sys/param.sfo: tools/package.py
	python3 tools/package.py sfo
assets/covers.pack assets/covers.idx: tools/pack_covers.py assets/catalog.tsv $(wildcard assets/covers/*.jpg)
	python3 tools/pack_covers.py
pkg.gp4: icons assets/covers.pack assets/covers.idx assets/brand/nullstore-bag.png assets/brand/store-icon.png assets/brand/service-icon.png assets/brand/ps4-case-frame.png assets/service/compat-jb.prx assets/service/COMPAT-PROVENANCE.txt assets/service/GOLDHEN-SDK-LICENSE.txt assets/service/ITEMZFLOW-LICENSE.txt assets/service/eboot.bin assets/service/daemon.sfo eboot.bin sce_sys/param.sfo tools/package.py assets/catalog.tsv assets/font.ttf sce_sys/icon0.png $(wildcard assets/covers/*.jpg)
	python3 tools/package.py gp4
build/$(CONTENT_ID).pkg: pkg.gp4
	python3 tools/package.py build $(PKGTOOL)
snapshot: build/catalog_parser
	python3 tools/scrape_sources.py --all
preview: build/preview
	./build/preview
build/preview: src/preview.cpp src/ui.h src/catalog.h src/providers.h src/download_model.h src/installer.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O2 -Wno-unused-function -idirafter $(OO_PS4_TOOLCHAIN)/include src/preview.cpp -o $@
build/torrent_host.o: src/torrent.c src/data_root.h src/torrent.h src/torrent_resume.h src/torrent_checkpoint.h src/path_info.h
	/usr/bin/clang -DPORTAL_HOST -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -fsanitize=address,undefined -c $< -o $@
build/torrent_driver: tests/torrent_driver.c build/torrent_host.o
	/usr/bin/clang -std=c11 -O1 -g -fsanitize=address,undefined $^ -o $@
build/test_torrent_unit: tests/test_torrent_unit.c src/torrent.c src/torrent.h src/torrent_resume.h src/torrent_checkpoint.h src/path_info.h
	/usr/bin/clang -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -fsanitize=address,undefined $< -o $@
build/test_torrent_socket: tests/test_torrent_socket.c src/torrent.c src/torrent.h src/net_compat.h src/torrent_resume.h src/torrent_checkpoint.h src/path_info.h
	/usr/bin/clang -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -fsanitize=address,undefined $< -o $@
build/test_download_manager: tests/test_download_manager.cpp src/download_manager.cpp src/download_manager.h src/download_model.h src/queue_store.h build/torrent_host.o
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/test_download_manager.cpp src/download_manager.cpp build/torrent_host.o -lpthread -o $@
build/queue_driver: tests/queue_driver.cpp src/download_manager.cpp src/download_manager.h src/queue_store.h build/torrent_host.o
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/queue_driver.cpp src/download_manager.cpp build/torrent_host.o -lpthread -o $@
build/test_installer: tests/test_installer.cpp src/installer.cpp src/installer.h src/queue_store.h src/path_info.h src/install_log.h src/bgft_abi.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/test_installer.cpp src/installer.cpp -lpthread -o $@
test: build/controller-driver build/test_service_permissions build/test_providers build/torrent_driver build/test_torrent_unit build/test_torrent_socket build/test_download_manager build/queue_driver build/test_installer build/portal-service-host build/service-client-driver
	python3 -m unittest discover -s tests -v
	./build/test_service_permissions
	./build/test_providers
	./build/test_torrent_unit
	./build/test_torrent_socket
	./build/test_download_manager
	./build/test_installer
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include tests/test_catalog.cpp -o build/test_catalog
	./build/test_catalog
validate: build/$(CONTENT_ID).pkg
	$(PKGTOOL) pkg_validate $<
	python3 tools/verify_package.py $< --tool $(PKGTOOL) --catalog

build/portal-service-host: src/service_main.cpp src/service_server.cpp src/service_protocol.cpp src/download_manager.cpp src/providers.h src/catalog.h src/download_model.h src/service_protocol.h src/service_server.h src/download_manager.h src/queue_store.h build/torrent_host.o
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined src/service_main.cpp src/service_server.cpp src/service_protocol.cpp src/download_manager.cpp build/torrent_host.o -lpthread -o $@
build/service-client-driver: tests/service_client_driver.cpp src/service_client.cpp src/service_client.h src/service_protocol.cpp src/service_protocol.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/service_client_driver.cpp src/service_client.cpp src/service_protocol.cpp -lpthread -o $@

build/catalog_parser: tools/catalog_parser.cpp src/catalog.h src/providers.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O2 $< -o $@
build/test_providers: tests/test_providers.cpp src/providers.h src/catalog.h src/download_model.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined $< -o $@
build/test_service_permissions: tests/test_service_permissions.cpp src/service_start_ps4.cpp src/service_protocol.cpp src/service_files.h src/service_setup_log.h src/path_info.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include tests/test_service_permissions.cpp src/service_protocol.cpp -lpthread -o $@
build/controller-driver: tests/controller_driver.cpp src/download_controller.cpp src/download_controller.h src/service_client.cpp src/download_manager.cpp src/service_protocol.cpp src/queue_store.h src/catalog.h src/providers.h build/torrent_host.o
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/controller_driver.cpp src/download_controller.cpp src/service_client.cpp src/download_manager.cpp src/service_protocol.cpp build/torrent_host.o -lpthread -o $@

-include $(wildcard build/*.d)
$(wildcard build/*.o): Makefile
build/service_installer.o: src/service_installer.cpp src/display_ps4.h src/ui.h src/providers.h src/service_start_ps4.h
	$(CC) $(CFLAGS) -c $< -o $@
build/service-installer.elf: tools/link_ps4.py build/service_installer.o build/service_start_ps4.o build/service_protocol.o build/network_ps4.o
	python3 tools/link_ps4.py $(LD) --no-mmap-output-file -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x --eh-frame-hdr -L$(OO_PS4_TOOLCHAIN)/lib $(OO_PS4_TOOLCHAIN)/lib/crt1.o $(filter %.o,$^) $(LIBS) -o $@
build/service-installer.bin: build/service-installer.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(CREATE_FSELF) -in=$< -out=build/service-installer.oelf --eboot $@ --paid 0x3800000000000011
build/IV0000-NULL00002_00-NULLSTORSERVICE0.pkg: icons build/service-installer.bin assets/service/eboot.bin assets/service/daemon.sfo assets/service/compat-jb.prx assets/service/ITEMZFLOW-LICENSE.txt assets/brand/service-icon.png tools/package_service_installer.py tools/package.py
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) python3 tools/package_service_installer.py
.PHONY: service-package
service-package: build/IV0000-NULL00002_00-NULLSTORSERVICE0.pkg

build/test_provider_cache: tests/test_provider_cache.cpp src/providers.h src/queue_store.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined $< -o $@
test: test-provider-cache
.PHONY: test-provider-cache
test-provider-cache: build/test_provider_cache
	./build/test_provider_cache

build/test_service_files: tests/test_service_files.cpp src/service_files.h src/path_info.h src/queue_store.h
	/usr/bin/clang++ -std=c++11 -O1 -g -fsanitize=address,undefined $< -o $@
build/test_file_info: tests/test_file_info.c src/path_info.h
	/usr/bin/clang -D_DARWIN_C_SOURCE -std=c11 -O1 -g -fsanitize=address,undefined $< -o $@
test: test-service-files
.PHONY: test-service-files
test-service-files: build/test_service_files build/test_file_info
	./build/test_service_files
	./build/test_file_info

build/test_installer_ps4: tests/test_installer_ps4.cpp src/installer_ps4.cpp src/installer.h src/bgft_abi.h src/install_log.h
	/usr/bin/clang++ -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@
build/test_installer_retry: tests/test_installer_retry.cpp src/installer.cpp src/installer.h src/bgft_abi.h src/queue_store.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/test_installer_retry.cpp src/installer.cpp -lpthread -o $@
.PHONY: test-installer-adapter
test: test-installer-adapter
build/test_startup_ps4: tests/test_startup_ps4.cpp src/startup_ps4.h src/install_log.h
	/usr/bin/clang++ -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@
build/test_installer_progress: tests/test_installer_progress.cpp src/installer.cpp src/installer.h src/queue_store.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined tests/test_installer_progress.cpp src/installer.cpp -lpthread -o $@
test-installer-adapter: build/test_installer_ps4 build/test_installer_retry build/test_startup_ps4 build/test_installer_progress
	./build/test_installer_ps4
	./build/test_installer_retry
	./build/test_startup_ps4
	./build/test_installer_progress

build/test_catalog: tests/test_catalog.cpp src/ui.h src/installer.h src/providers.h src/path_info.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@

build/test_queue_handoff: tests/test_queue_handoff.cpp src/download_controller.cpp src/download_controller.h src/service_client.cpp src/service_client.h src/download_manager.cpp src/download_manager.h src/service_protocol.cpp src/queue_store.h build/torrent_host.o
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include tests/test_queue_handoff.cpp src/download_controller.cpp src/service_client.cpp src/download_manager.cpp src/service_protocol.cpp build/torrent_host.o -lpthread -o $@

.PHONY: test-queue-handoff
test: test-queue-handoff
test-queue-handoff: build/test_queue_handoff build/portal-service-host
	./build/test_queue_handoff "$(CURDIR)/build/portal-service-host"

build/text_input_ps4.o: src/text_input_ps4.cpp src/text_input_ps4.h src/network_ps4.h
	$(CC) $(CFLAGS) -c $< -o $@

build/test_text_input: tests/test_text_input.cpp src/text_input_ps4.cpp src/text_input_ps4.h
	/usr/bin/clang++ -DPORTAL_HOST -std=c++11 -O1 -g -fsanitize=address,undefined -idirafter $(OO_PS4_TOOLCHAIN)/include $< -o $@

.PHONY: test-text-input
test: test-text-input
test-text-input: build/test_text_input
	./build/test_text_input
