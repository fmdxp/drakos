#include "drk.hpp"
#include "vfs.hpp"
#include "elf.hpp"
#include "thread.hpp"
#include "panic.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"

using namespace DRK;

DrkLoader::DrkLoader() : filePath(nullptr), fd(-1), loaded(false), valid(false) {
    for (size_t i = 0; i < sizeof(header); i++) ((uint8_t*)&header)[i] = 0;
}

DrkLoader::~DrkLoader() {
    if (fd >= 0) {
        vfs_close(fd);
    }
}

bool DrkLoader::load(const char* path) {
    filePath = path;
    fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        if (g_vga) g_vga->write("DrkLoader: Failed to open file\n");
        return false;
    }

    if (!parseHeader()) return false;
    if (!validate()) return false;
    
    // Only NativeELF64 is supported for execution right now
    if (header.executableType != DrkExeType::NativeELF64) {
        if (g_vga) g_vga->write("DrkLoader: Unsupported executable type\n");
        return false;
    }

    if (!loadExecutable()) return false;

    loaded = true;
    return true;
}

bool DrkLoader::parseHeader() {
    vfs_seek(fd, 0, VFS_SEEK_SET);
    int read_bytes = vfs_read(fd, &header, sizeof(DrkHeader));
    if (read_bytes != sizeof(DrkHeader)) {
        if (g_vga) g_vga->write("DrkLoader: Failed to read header\n");
        return false;
    }
    return true;
}

bool DrkLoader::validateMagic() {
    return header.drkMagic == DRK_MAGIC;
}

bool DrkLoader::validateOffsets() {
    return header.executableOffset >= sizeof(DrkHeader);
}

bool DrkLoader::validateSizes() {
    return header.executableSize > 0;
}

bool DrkLoader::verifyHash() {
    // TODO: implement
    return true;
}

bool DrkLoader::verifySignature() {
    // TODO: implement
    return true;
}

bool DrkLoader::checkVersion() {
    return header.drkVersion >= DRK_VERSION;
}

bool DrkLoader::checkRequirements() {
    return true;
}

bool DrkLoader::checkSecurity() {
    return true;
}

bool DrkLoader::validate() {
    if (!validateMagic() || !validateOffsets() || !validateSizes()) return false;
    if (!verifyHash() || !verifySignature()) return false;
    if (!checkRequirements() || !checkSecurity()) return false;
    valid = true;
    return true;
}

bool DrkLoader::loadExecutable() {
    // Read ELF Header
    Elf64_Ehdr ehdr;
    vfs_seek(fd, header.executableOffset, VFS_SEEK_SET);
    if (vfs_read(fd, &ehdr, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) return false;

    // Verify ELF Magic
    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' || 
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        if (g_vga) g_vga->write("DrkLoader: Invalid ELF magic\n");
        return false;
    }

    if (ehdr.e_machine != EM_X86_64) {
        if (g_vga) g_vga->write("DrkLoader: Not an x86_64 ELF\n");
        return false;
    }

    // Create process (Thread constructor will create the address space)
    Process* proc = new Process();

    // Create Thread FIRST — this sets up proc->page_table_phys and the user stack
    Thread* user_thread = new Thread(proc, (void(*)())ehdr.e_entry, true, "DrkApp");

    // Now we have proc->page_table_phys — map ELF segments into it
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        vfs_seek(fd, header.executableOffset + ehdr.e_phoff + (i * ehdr.e_phentsize), VFS_SEEK_SET);
        vfs_read(fd, &phdr, sizeof(Elf64_Phdr));

        if (phdr.p_type == PT_LOAD) {
            uint64_t virt_base        = phdr.p_vaddr & ~(PAGE_SIZE - 1);
            uint64_t offset_in_page   = phdr.p_vaddr & (PAGE_SIZE - 1);
            uint64_t remaining_file   = phdr.p_filesz;
            uint64_t file_offset      = phdr.p_offset;
            uint64_t mem_pages        = (phdr.p_memsz + offset_in_page + PAGE_SIZE - 1) / PAGE_SIZE;

            uint64_t flags = VMM_PRESENT | VMM_USER;
            if (phdr.p_flags & PF_W) flags |= VMM_WRITE;
            // Note: executable pages have NX bit CLEAR (i.e., no VMM_NX)

            for (uint64_t p = 0; p < mem_pages; p++) {
                uintptr_t phys  = pmm_alloc_page();
                uint64_t  vaddr = virt_base + (p * PAGE_SIZE);

                vmm_map_page(proc->page_table_phys, vaddr, phys, flags);

                // Zero the page first (.bss gets clean memory)
                uint8_t* virt_ptr = (uint8_t*)(phys + pmm_hhdm_offset());
                for (size_t j = 0; j < PAGE_SIZE; j++) virt_ptr[j] = 0;

                // Copy file data if available
                if (remaining_file > 0) {
                    uint64_t copy_off  = (p == 0) ? offset_in_page : 0;
                    uint64_t read_size = PAGE_SIZE - copy_off;
                    if (read_size > remaining_file) read_size = remaining_file;

                    vfs_seek(fd, header.executableOffset + file_offset, VFS_SEEK_SET);
                    vfs_read(fd, virt_ptr + copy_off, read_size);

                    remaining_file -= read_size;
                    file_offset    += read_size;
                }
            }
        }
    }

    scheduler_add_thread(user_thread);
    return true;
}

bool DrkLoader::loadAssets() { return true; }
bool DrkLoader::loadMetadata() { return true; }
bool DrkLoader::prepareExecution() { return true; }
bool DrkLoader::isValid() const { return valid; }
bool DrkLoader::isLoaded() const { return loaded; }
const DrkHeader& DrkLoader::getHeader() const { return header; }
