#
# drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
# Copyright (C) 2026 fmdxp
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#


## Compilers ##

CROSS_COMPILE ?= x86_64-elf-
CXX = $(CROSS_COMPILE)g++
LD  = $(CROSS_COMPILE)ld
LIMINE ?= limine
LIMINE_CONF ?= limine.conf


## Dirs ##

LIMINE_DIR ?= $(shell data_dir=`$(LIMINE) --print-datadir 2>/dev/null`; \
	if test -n "$$data_dir" && test -f "$$data_dir/BOOTX64.EFI"; then echo "$$data_dir"; \
	else limine_path=`command -v $(LIMINE)`; prefix=`dirname "$$(dirname "$$limine_path")"`; \
		candidate="$$prefix/share/limine"; \
		if test -f "$$candidate/BOOTX64.EFI"; then echo "$$candidate"; fi; fi)

KERNEL 		= iso_root/kernel.elf
ISO			= drakos.iso
BUILD 		= build

USB_IMG		= usb_stick.img
DISK_IMG	= disk.img
NVME_IMG	= nvme.img

USB_IMG_SIZE 	= 64M
DISK_IMG_SIZE 	= 512M
NVME_IMG_SIZE 	= 64M


SRCS_CPP = $(shell find src -name "*.cpp")
SRCS_ASM = $(shell find src -name "*.S")
OBJS = $(patsubst %.cpp, $(BUILD)/%.o, $(notdir $(SRCS_CPP))) \
       $(patsubst %.S, $(BUILD)/%.o, $(notdir $(SRCS_ASM)))
FONT_OBJ = $(BUILD)/font.o
vpath %.cpp $(sort $(dir $(SRCS_CPP)))
vpath %.S $(sort $(dir $(SRCS_ASM)))



## Flags ##

CXXFLAGS = 	-std=c++20 -O0 -fno-inline -Wall -Wextra \
			-ffreestanding \
			-fno-exceptions \
			-fno-rtti \
			-mno-red-zone \
			-mcmodel=kernel \
			-mgeneral-regs-only \
			-fstack-protector-strong \
			-fno-pic \
			-nostdlib \
			-Isrc \
			-Iinclude \
			-Iinclude/drivers \
			-Iinclude/kernel \
			-Iinclude/memory \
			-Iinclude/fs \
			-Iinclude/usb \
			-Iinclude/input \
			-Iinclude/drk \
			-g

LDFLAGS	=	-T linker.ld -nostdlib


## Targets ##

.PHONY: all clean run debug images test-initcalls test-font test-pmm test-vmm test-heap qemu-test-address-space

all: $(ISO)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -include include/kernel/stack.hpp -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@


$(BUILD)/font.o: src/fonts/Lat2-Terminus16.psf
	@mkdir -p $(BUILD)
	objcopy -O elf64-x86-64 -B i386 -I binary $< $@

$(KERNEL): $(OBJS) $(FONT_OBJ) linker.ld
	@mkdir -p iso_root
	$(LD) $(LDFLAGS) $(OBJS) $(FONT_OBJ) -o $(KERNEL)


$(ISO): $(KERNEL) $(LIMINE_CONF)
	@mkdir -p iso_root/EFI/BOOT
	cp $(LIMINE_DIR)/BOOTX64.EFI iso_root/EFI/BOOT/
	cp $(LIMINE_CONF) iso_root/limine.conf
	cp $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/
	xorriso -as mkisofs -R -r -J -V "DRAKOS" \
		--efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
		iso_root -o $(ISO)


images: $(USB_IMG) $(DISK_IMG) $(NVME_IMG)
	@true

$(USB_IMG):
	@echo "[drakos] Creating USB image..."
	fallocate -l $(USB_IMG_SIZE) $@ || dd if=/dev/zero of=$@ bs=1M count=64
	mkfs.fat -F 32 $@
	chmod 666 $@
	@echo "This is the USB stick!" > build/USB.TXT
	mcopy -o -i $@ build/USB.TXT ::/USB.TXT
	@rm -f build/USB.TXT

$(DISK_IMG):
	@echo "[drakos] Creating DISK image..."
	fallocate -l $(DISK_IMG_SIZE) $@ || dd if=/dev/zero of=$@ bs=1M count=512
	mkfs.fat -F 32 $@
	chmod 666 $@
	@echo "Hello from drakos VFS!" > build/HELLO.TXT
	mcopy -o -i $@ build/HELLO.TXT ::/HELLO.TXT
	@rm -f build/HELLO.TXT
	@echo "[drakos] Copying Intel BT Firmware..."
	@find /usr/lib/firmware/intel -name "ibt-*.sfi.zst" ! -name "*-iml*" ! -name "*-pci*" ! -name "*-usb*" -exec bash -c 'for f; do base=$$(basename "$$f" .sfi.zst); short=$${base#ibt-}; short=$${short//-/}; short=$${short:0:8}; zstd -d -c -f -q "$$f" > "build/$$short.SFI"; mcopy -o -i $@ "build/$$short.SFI" ::/; done' _ {} + || true


# NVMe image: always recreated fresh whenever hello.drk changes
$(NVME_IMG): userspace/hello.drk
	@echo "[drakos] Creating NVMe image..."
	fallocate -l $(NVME_IMG_SIZE) $@ || dd if=/dev/zero of=$@ bs=1M count=64
	mkfs.fat -F 32 $@
	chmod 666 $@
	mcopy -o -i $@ userspace/hello.drk ::/hello.drk
	@sync
	@echo "[drakos] NVMe seeded with hello.drk"

.PHONY: FORCE build_userspace clean_userspace

build_userspace:
	@$(MAKE) -C userspace --no-print-directory

userspace/hello.drk: FORCE
	@$(MAKE) -C userspace --no-print-directory

seed_disk: images
	@echo "[drakos] All disks ready."



clean_userspace:
	@$(MAKE) -C userspace clean --no-print-directory

clean: clean_userspace
	rm -rf $(BUILD) $(ISO) iso_root $(USB_IMG) $(DISK_IMG) $(NVME_IMG)


run: $(ISO) seed_disk
	@sudo chmod 666 $(DISK_IMG) $(USB_IMG) $(NVME_IMG) 2>/dev/null || true
	qemu-system-x86_64 -cpu max -bios /usr/share/ovmf/OVMF.fd -cdrom drakos.iso -m 256M -serial stdio \
        -device qemu-xhci,id=xhci \
        -device usb-host,bus=xhci.0,vendorid=0x054c,productid=0x0ce6 \
        -device usb-host,bus=xhci.0,vendorid=0x8087,productid=0x0026 \
        -device usb-kbd,bus=xhci.0 \
        -drive id=usbdisk,file=usb_stick.img,if=none,format=raw,file.locking=off \
        -device usb-storage,bus=xhci.0,drive=usbdisk \
        -drive id=disk,file=disk.img,if=none,format=raw,file.locking=off \
        -device ahci,id=ahci \
        -device ide-hd,drive=disk,bus=ahci.0 \
        -drive id=nvme0,file=nvme.img,if=none,format=raw,file.locking=off \
        -device nvme,serial=NVME1234,drive=nvme0


qemu-test-address-space: $(ISO)
	@set -eu; test_root=$$(mktemp -d /tmp/drakos-as-test.XXXXXX); \
	trap 'rm -rf "$$test_root"' EXIT; \
	cp -a iso_root "$$test_root/root"; \
	sed 's/cmdline: drakos.mode=normal/cmdline: drakos.mode=verbose drakos.test=address-space/' limine.conf > "$$test_root/root/limine.conf"; \
	xorriso -as mkisofs -R -r -J -V DRAKOS-AS-TEST --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image "$$test_root/root" -o "$$test_root/test.iso" >/dev/null 2>&1; \
	truncate -s 32M "$$test_root/disk.img"; \
	set +e; timeout 20s qemu-system-x86_64 -cpu max -smp 4 -bios /usr/share/ovmf/OVMF.fd -cdrom "$$test_root/test.iso" -m 256M -display none -serial "file:$$test_root/serial.log" -monitor none -no-reboot -device ahci,id=ahci -drive "file=$$test_root/disk.img,format=raw,if=none,id=disk" -device ide-hd,bus=ahci.0,drive=disk; qemu_status=$$?; set -e; \
	if [ "$$qemu_status" -ne 124 ]; then cat "$$test_root/serial.log"; echo "QEMU exited unexpectedly: $$qemu_status"; exit 1; fi; \
	cat "$$test_root/serial.log"; grep -q 'address-space test: PASS' "$$test_root/serial.log"

debug: $(ISO) images seed_disk
	qemu-system-x86_64 \
		-cpu max \
		-bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(ISO) \
		-m 256M \
		-display sdl \
		-serial stdio \
		-device qemu-xhci,id=xhci \
		-device usb-host,bus=xhci.0,vendorid=0x054c,productid=0x0ce6 \
		-device usb-kbd,bus=xhci.0 \
		-drive id=disk,file=$(DISK_IMG),if=none,format=raw \
		-device ahci,id=ahci \
		-device ide-hd,drive=disk,bus=ahci.0 \
		-drive id=nvme0,file=$(NVME_IMG),if=none,format=raw \
		-device nvme,serial=NVME1234,drive=nvme0 \
		-s -S

HOST_CXX ?= g++
build/initcall_registry_tests: tests/initcall_registry_test.cpp src/kernel/initcall_registry.cpp include/kernel/initcall_registry.hpp
	@mkdir -p $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -Iinclude $< src/kernel/initcall_registry.cpp -o $@

test-initcalls: build/initcall_registry_tests
	./build/initcall_registry_tests

build/font_unicode_tests: tests/font_unicode_test.cpp include/font.hpp src/fonts/Lat2-Terminus16.psf
	@mkdir -p $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -Iinclude $< -o $@

test-font: build/font_unicode_tests
	./build/font_unicode_tests src/fonts/Lat2-Terminus16.psf

build/pmm_tests: tests/pmm_test.cpp src/memory/pmm.cpp include/memory/pmm.hpp include/kernel/limine_requests.hpp
	@mkdir -p $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -DDRAKOS_HOST_TEST -Iinclude -Iinclude/memory -Iinclude/kernel $< src/memory/pmm.cpp -o $@

test-pmm: build/pmm_tests
	./build/pmm_tests

build/vmm_tests: tests/vmm_test.cpp src/memory/pmm.cpp src/memory/vmm.cpp include/memory/pmm.hpp include/memory/vmm.hpp include/kernel/limine_requests.hpp
	@mkdir -p $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -DDRAKOS_HOST_TEST -Iinclude -Iinclude/memory -Iinclude/kernel $< src/memory/pmm.cpp src/memory/vmm.cpp -o $@

test-vmm: build/vmm_tests
	./build/vmm_tests

build/heap_tests: tests/heap_test.cpp src/memory/heap.cpp include/memory/heap.hpp include/memory/pmm.hpp include/memory/vmm.hpp
	@mkdir -p $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -DDRAKOS_HOST_TEST -Iinclude -Iinclude/memory $< src/memory/heap.cpp -o $@

test-heap: build/heap_tests
	./build/heap_tests
