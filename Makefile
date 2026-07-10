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

CXX = cross/bin/x86_64-elf-g++
LD  = cross/bin/x86_64-elf-ld


## Dirs ##

LIMINE_DIR 	= ./limine-binary

KERNEL 		= iso_root/kernel.elf
ISO			= drakos.iso
BUILD 		= build

USB_IMG		= usb_stick.img
DISK_IMG	= disk.img
NVME_IMG	= nvme.img
STAGE_DIR 	= stage_disk

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

CXXFLAGS = 	-std=c++20 -O2 -Wall -Wextra \
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

.PHONY: all clean run debug images

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


$(ISO): $(KERNEL) limine.conf
	@mkdir -p iso_root/EFI/BOOT
	cp $(LIMINE_DIR)/BOOTX64.EFI iso_root/EFI/BOOT/
	cp limine.conf iso_root/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/
	xorriso -as mkisofs -R -r -J -V "DRAKOS" \
		--efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
		iso_root -o $(ISO)


images:
	@if [ ! -f $(USB_IMG) ]; then \
		echo "[drakos] creating USB image..."; \
		fallocate -l $(USB_IMG_SIZE) $(USB_IMG) || dd if=/dev/zero of=$(USB_IMG) bs=1M count=64; \
		mkfs.fat -F 32 $(USB_IMG); \
	else \
		echo "[drakos] USB image already exists"; \
	fi

	@if [ ! -f $(DISK_IMG) ]; then \
		echo "[drakos] Creating DISK image..."; \
		fallocate -l $(DISK_IMG_SIZE) $(DISK_IMG) || dd if=/dev/zero of=$(DISK_IMG) bs=1M count=512; \
		mkfs.fat -F 32 $(DISK_IMG); \
	else \
		echo "[drakos] DISK image already exists"; \
	fi

	@if [ ! -f $(NVME_IMG) ]; then \
		echo "[drakos] Creating NVME image..."; \
		fallocate -l $(NVME_IMG_SIZE) $(NVME_IMG) || dd if=/dev/zero of=$(NVME_IMG) bs=1M count=64; \
		mkfs.fat -F 32 $(NVME_IMG); \
	else \
		echo "[drakos] NVME image already exists"; \
	fi


seed_disk: images
	@echo "[drakos] seeding disk..."
	@rm -rf $(STAGE_DIR) || sudo rm -rf $(STAGE_DIR)
	@mkdir -p $(STAGE_DIR)
	@echo "Hello from drakos VFS!" > $(STAGE_DIR)/HELLO.TXT
	@mcopy -o -i $(DISK_IMG) $(STAGE_DIR)/HELLO.TXT ::/HELLO.TXT
	@echo "This is the USB stick!" > $(STAGE_DIR)/USB.TXT
	@mcopy -o -i $(USB_IMG) $(STAGE_DIR)/USB.TXT ::/USB.TXT
	@$(MAKE) -C userspace
	@mcopy -o -i $(NVME_IMG) userspace/hello.drk ::/hello.drk
	@sync
	@sleep 0.1
	@echo "[drakos] disk and usb seeded"



clean:
	sudo rm -rf $(BUILD) $(ISO) iso_root $(STAGE_DIR) $(USB_IMG) $(DISK_IMG) $(NVME_IMG)


run: $(ISO) seed_disk
	@sudo chmod 666 $(DISK_IMG) $(USB_IMG) $(NVME_IMG) 2>/dev/null || true
	qemu-system-x86_64 -cpu max -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO) -m 256M -display sdl -serial stdio \
		-device qemu-xhci,id=xhci \
		-device usb-host,bus=xhci.0,vendorid=0x054c,productid=0x0ce6 \
		-device usb-kbd,bus=xhci.0 \
		-drive id=usbdisk,file=usb_stick.img,if=none,format=raw \
		-device usb-storage,bus=xhci.0,drive=usbdisk \
		-drive id=disk,file=disk.img,if=none,format=raw \
		-device ahci,id=ahci \
		-device ide-hd,drive=disk,bus=ahci.0 \
		-drive id=nvme0,file=$(NVME_IMG),if=none,format=raw \
		-device nvme,serial=NVME1234,drive=nvme0


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