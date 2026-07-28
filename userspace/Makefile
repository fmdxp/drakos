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


CXX = ../cross/bin/x86_64-elf-g++
CC = ../cross/bin/x86_64-elf-gcc
AS = ../cross/bin/x86_64-elf-as
LD = ../cross/bin/x86_64-elf-ld
PACKAGER = ../tools/drk-packager/drk-packager

CFLAGS = -O2 -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -nostdlib -ffreestanding -I./libc/include
CXXFLAGS = -std=c++20 $(CFLAGS)

TARGET_ELF = hello.elf
TARGET_DRK = hello.drk

SRCS_C = $(wildcard libc/*.c)
SRCS_CXX = hello.cpp
SRCS_S = $(wildcard libc/*.S)

OBJS = $(SRCS_C:.c=.o) $(SRCS_CXX:.cpp=.o) $(SRCS_S:.S=.o)

all: $(TARGET_DRK)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET_ELF): $(OBJS)
	$(LD) -T user.ld -o $(TARGET_ELF) $(OBJS)

$(TARGET_DRK): $(TARGET_ELF)
	$(PACKAGER) $(TARGET_ELF) $(TARGET_DRK)

clean:
	rm -f *.o $(TARGET_ELF) $(TARGET_DRK)
