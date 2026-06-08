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


SRCS = $(shell find src -name "*.cpp")
OBJS = $(patsubst %.cpp, $(BUILD)/%.o, $(notdir $(SRCS)))
FONT_OBJ = $(BUILD)/font.o
vpath %.cpp $(sort $(dir $(SRCS)))



## Flags ##

CXXFLAGS = 	-std=c++20 -O2 -Wall -Wextra \
			-ffreestanding \
			-fno-exceptions \
			-fno-rtti \
			-mno-red-zone \
			-mcmodel=kernel \
			-mgeneral-regs-only \
			-fno-stack-protector \
			-fno-pic \
			-nostdlib \
			-Isrc \
			-Iinclude \
			-Iinclude/drivers \
			-Iinclude/kernel \
			-Iinclude/memory 

LDFLAGS	=	-T linker.ld -nostdlib



## Targets ##

.PHONY: all clean run

all: $(ISO)

$(BUILD)/%.o: %.cpp
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


clean:
	rm -rf $(BUILD) iso_root/kernel.elf iso_root/BOOTX64.EFI $(ISO)


run: $(ISO)
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO) -m 256M -display sdl -serial stdio -device qemu-xhci,id=xhci -device usb-host,bus=xhci.0,vendorid=0x054c,productid=0x0ce6 -device usb-kbd,bus=xhci.0