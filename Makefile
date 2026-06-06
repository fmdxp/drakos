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
vpath %.cpp $(sort $(dir $(SRCS)))



## Flags ##

CXXFLAGS = 	-std=c++20 -O2 -Wall -Wextra \
			-ffreestanding \
			-fno-exceptions \
			-fno-rtti \
			-mno-red-zone \
			-mcmodel=kernel \
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


$(KERNEL): $(OBJS) linker.ld
	@mkdir -p iso_root
	$(LD) $(LDFLAGS) $(OBJS) -o $(KERNEL)


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
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO) -m 256M -display sdl