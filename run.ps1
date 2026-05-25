$ErrorActionPreference = "Stop"

# 1. Compile the project
Write-Host "Installing the rust target for UEFI..." -ForegroundColor Cyan
rustup target add x86_64-unknown-uefi

Write-Host "Compiling the project for UEFI..." -ForegroundColor Cyan
cargo build

if ($LASTEXITCODE -ne 0) {
    Write-Host "Compilation error!" -ForegroundColor Red
    exit 1
}

# 2. Create the directory for the EFI System Partition (ESP)
$esp_dir = "target\esp"
$boot_dir = "$esp_dir\EFI\BOOT"

if (!(Test-Path -Path $boot_dir)) {
    New-Item -ItemType Directory -Force -Path $boot_dir | Out-Null
}

# 3. Copy the compiled executable
# Note: The executable takes its name from the package in Cargo.toml ("drakos")
$compiled_efi = "target\x86_64-unknown-uefi\debug\drakos.efi"
$target_efi = "$boot_dir\BOOTX64.EFI"

Copy-Item -Path $compiled_efi -Destination $target_efi -Force
Write-Host "EFI file copied to $target_efi" -ForegroundColor Green

# 4. Download OVMF (UEFI Firmware for QEMU) if it does not exist
$ovmf = "OVMF.fd"
if (!(Test-Path -Path $ovmf)) {
    Write-Host "Downloading OVMF.fd (UEFI Firmware)..." -ForegroundColor Yellow
    Invoke-WebRequest -Uri "https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd" -OutFile $ovmf
    Write-Host "Download completed!" -ForegroundColor Green
}

# 5. Start QEMU
Write-Host "Starting QEMU..." -ForegroundColor Cyan
# Pass the OVMF.fd firmware as flash memory
# And mount the target/esp folder as a FAT drive
& "C:\Program Files\qemu\qemu-system-x86_64.exe" `
    -drive if=pflash,format=raw,readonly=on,file=$ovmf `
    -drive format=raw,file=fat:rw:$esp_dir `
    -vga std `
    -m 256M `
    -net none
