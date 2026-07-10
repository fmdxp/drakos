#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include "../../include/drk/elf.hpp"

using namespace DRK;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: drk-packager <input.elf> <output.drk>\n";
        return 1;
    }

    const char* input_path = argv[1];
    const char* output_path = argv[2];

    std::ifstream infile(input_path, std::ios::binary);
    if (!infile) {
        std::cerr << "Failed to open input file: " << input_path << "\n";
        return 1;
    }

    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    infile.close();

    DrkHeader header = {};
    memset(&header, 0, sizeof(header));
    
    header.drkMagic = DRK_MAGIC;
    header.drkVersion = DRK_VERSION;
    header.minimumOSVersion = DRK_VERSION;
    
    header.headerSize = sizeof(DrkHeader);
    
    header.type = DrkType::App;
    header.executableType = DrkExeType::NativeELF64;
    header.storagePolicy = DrkStorage::Any;
    header.corePolicy = DrkCorePolicy::Shared;
    
    header.flags = DrkFlags::None;
    header.securityFlags = DrkSecurityFlags::None;
    
    header.capabilities = DrkCapabilities::Gamepad; // Default capability
    header.minimumCPUFeatures = DrkMinimumCPUFeatures::SSE2;
    
    header.titleId = 0x12345678; // Dummy Title ID
    
    // For NativeELF64, the load address and entry point will be read from the ELF header by the kernel.
    header.preferredLoadAddress = 0; 
    header.stackSize = 1024 * 1024 * 2; // 2MB stack
    
    header.executableOffset = sizeof(DrkHeader);
    header.executableSize = payload.size();
    
    // Write out the file
    std::ofstream outfile(output_path, std::ios::binary);
    if (!outfile) {
        std::cerr << "Failed to open output file: " << output_path << "\n";
        return 1;
    }
    
    outfile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    outfile.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    outfile.close();
    
    std::cout << "Successfully packaged " << payload.size() << " bytes into " << output_path << "\n";
    return 0;
}
