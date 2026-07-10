CXX = ../cross/bin/x86_64-elf-g++
LD = ../cross/bin/x86_64-elf-ld
PACKAGER = ../tools/drk-packager/drk-packager

CXXFLAGS = -std=c++20 -O2 -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -nostdlib -ffreestanding

TARGET_ELF = hello.elf
TARGET_DRK = hello.drk
SRCS = hello.cpp

all: $(TARGET_DRK)

$(TARGET_ELF): $(SRCS)
	$(CXX) $(CXXFLAGS) -c $(SRCS) -o hello.o
	$(LD) -T user.ld -o $(TARGET_ELF) hello.o

$(TARGET_DRK): $(TARGET_ELF)
	$(PACKAGER) $(TARGET_ELF) $(TARGET_DRK)

clean:
	rm -f *.o $(TARGET_ELF) $(TARGET_DRK)
