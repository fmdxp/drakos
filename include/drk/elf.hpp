/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#pragma once

#include <stdint.h>
#include <stddef.h>

#pragma pack(push,1)

namespace DRK
{
    #define DRK_VERSION_MAJOR 1
    #define DRK_VERSION_MINOR 0
    #define DRK_VERSION_PATCH 0

    #define DRK_VERSION ((DRK_VERSION_MAJOR << 16) | (DRK_VERSION_MINOR << 8) | DRK_VERSION_PATCH)


    inline uint32_t patchToNumber(const char* patch)
    {
        uint32_t value = 0;

        while (*patch)
        {
            value = value * 26 + (*patch - 'a' + 1);
            patch++;
        }

        return value - 1;
    }


    constexpr uint32_t DRK_MAGIC = 0x44524B00;      // "DRK" 0 Terminated


    enum class DrkType : uint8_t
    {
        Game    = 0,
        App     = 1,        // App and Game are for developers
        System  = 2         // Like the Settings app or the Store -> System Apps only!
    };


    enum class DrkFlags : uint32_t
    {
        None            = 0,
        
        HasSaveData     = 1 << 0,               // Game/App saves its data on the system (requires Storage as Capability)
        HasAssets       = 1 << 1,               // Assets embedded in the .drk

        RequiresVSync   = 1 << 2,
        AllowJIT        = 1 << 3,

        IsBackgroundSuspendable     = 1 << 4,

        RequiresPhysicalMapping     = 1 << 5,
        UsesHugePages               = 1 << 6,

        EnableWriteCombining        = 1 << 7,
        RequireNonVolatileStorage   = 1 << 8      // Game/App can't run on USBs or SD Cards
    };


    enum class DrkSecurityFlags : uint32_t
    {
        None = 0,

        Verified        = 1 << 0,
        Protected       = 1 << 1,
        EnforceW_X      = 1 << 2,
        EnableASLR      = 1 << 3,
        
        NoDebug         = 1 << 4,
        AntiTamper      = 1 << 5,
        LimitHeapSize   = 1 << 6        // Puts a roof on the heap used -> Debugging purposes, EXTREMELY UNRECCOMENDED FOR RELEASES
    };


    enum class DrkCorePolicy : uint8_t
    {
        Shared,
        PreferDedicated,
        Exclusive
    };


    enum class DrkMinimumCPUFeatures : uint64_t
    {
        SSE2        = 1 << 0,
        SSE3        = 1 << 1,
        SSE4_1      = 1 << 2,
        SSE4_2      = 1 << 3,

        AVX         = 1 << 4,
        AVX2        = 1 << 5,
    };


    enum class DrkCapabilities : uint64_t
    {
        Vulkan            = 1 << 0,     // Unimplemented -> TODO for the future
        Draconic          = 1 << 1,     // System's gAPI

        Audio             = 1 << 2,
        Gamepad           = 1 << 3,
        Network           = 1 << 4,

        Microphone        = 1 << 5,     // Refers to the Game/App to be able to listen to the user's voice through an API
        Camera            = 1 << 6,     // Same as above but for Camera

        LocalMultiplayer  = 1 << 7,     // Enables more controllers in the app/game -> if not it explicitely tells the user
        Achievements      = 1 << 8,

        Storage           = 1 << 9      // For Game Saves or Screenshots
    };


    enum class DrkPerms: uint8_t
    {
        None        = 0,

        Execute     = 1 << 0,
        Write       = 1 << 1,
        Read        = 1 << 2
    };


    enum class DrkExeType: uint8_t
    {
        NativeELF64 = 0,
        EmulatorROM = 1,
        ByteCode    = 2,
    };


    enum class DrkStorage : uint8_t
    {
        Any             = 0,

        InternalOnly    = 1,    // NVMe/SATA
        ExternalAllowed = 2,    // USB, SD
        ReadOnlyAllowed = 3,    // CD/DVD
    };


    struct DrkHeader
    {
        uint32_t                    drkMagic;
        uint32_t                    drkVersion;

        uint32_t                    minimumOSVersion;

        uint16_t                    headerSize;
        uint16_t                    reserved0;

        DrkType                     type;
        DrkExeType                  executableType;
        DrkStorage                  storagePolicy;
        DrkCorePolicy               corePolicy;

        DrkFlags                    flags;
        DrkSecurityFlags            securityFlags;  
        uint32_t                    reserved1;

        DrkCapabilities             capabilities;
        DrkMinimumCPUFeatures       minimumCPUFeatures;

        uint64_t                    titleId;

        uint64_t                    preferredLoadAddress;
        uint64_t                    stackSize;

        uint64_t                    executableOffset;
        uint64_t                    executableSize;
        uint64_t                    entryPoint;

        uint64_t                    assetOffset;
        uint64_t                    assetSize;

        uint64_t                    metadataOffset;
        uint64_t                    metadataSize;

        uint64_t                    securityOffset;
        uint64_t                    securitySize;

        uint64_t                    hashOffset;
        uint64_t                    hashSize;
    
        uint8_t                     reserved2[32];      // Can use up to 4 new uint64_t
    };

    static_assert(sizeof(DrkHeader) == 192, "DrkHeader size changed!");



    class DrkLoader
    {
    public:
        DrkLoader();
        ~DrkLoader();

        bool load(const char* path);

        bool parseHeader();
        bool validate();

        bool checkVersion();
        bool checkRequirements();
        bool checkSecurity();

        bool loadExecutable();
        bool loadAssets();
        bool loadMetadata();

        bool prepareExecution();

        bool isValid() const;
        bool isLoaded() const;

        const DrkHeader& getHeader() const;

    private: 
        bool validateMagic();
        bool validateOffsets();
        bool validateSizes();
        bool verifyHash();
        bool verifySignature();

        const char* filePath;
        int fd;

        DrkHeader header;

        bool loaded;
        bool valid;
    };
}

#pragma pack(pop)