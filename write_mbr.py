import struct
with open('/home/dany/drakos/disk.img', 'r+b') as f:
    f.seek(510)
    b = f.read(2)
    print(f"Bytes 510-511: {hex(b[0])} {hex(b[1])}")
    if b[0] != 0x55 or b[1] != 0xAA:
        f.seek(510)
        f.write(b'\x55\xAA')
        print("MBR magic written!")
    else:
        print("MBR magic already present!")
