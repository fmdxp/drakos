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
			-fno-stack-protector \
			-fno-pic \
			-nostdlib \
			-Isrc \
			-Iinclude \

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


$(ISO): $(KERNEL) limine.cfg
	cp $(LIMINE_DIR)/BOOTX64.EFI iso_root/
	xorriso -as mkisofs -b BOOTX64.EFI \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		iso_root -o $(ISO)
	$(LIMINE_DIR)/limine bios-install $(ISO)


clean:
	rm -rf $(BUILD) iso_root/kernel.elf iso_root/BOOTX64.EFI $(ISO)


run: $(ISO)
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO) -m 256M