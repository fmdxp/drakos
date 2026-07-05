'''
drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
Copyright (C) 2026 fmdxp

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
'''


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
