#!/usr/bin/env python3
"""mkfat.py - build a FAT12/FAT16 disk image from a directory tree.

Used to create the initrd (RAM disk loaded by GRUB) and the optional
persistent hard-disk image, without needing root, mtools or mkfs.fat.

    mkfat.py <source-dir> <image> --size-kb 2048 [--fat 12|16] [--label NAME]

File names must be valid 8.3 names (e.g. HELLO, MOTD, README.TXT).
"""
import argparse, math, os, struct, sys, time

def name83(name):
    name = name.upper()
    base, _, ext = name.rpartition('.') if '.' in name else (name, '', '')
    ok = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~')
    if not base or len(base) > 8 or len(ext) > 3 or not set(base + ext) <= ok:
        sys.exit("mkfat: '%s' is not a valid 8.3 file name" % name)
    return (base.ljust(8) + ext.ljust(3)).encode()

def fat_datetime():
    t = time.localtime()
    date = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date, tm

def dirent(name, attr, cluster, size):
    date, tm = fat_datetime()
    return struct.pack('<11sBBBHHHHHHHI', name, attr, 0, 0, tm, date, date, 0, tm, date, cluster, size)

def layout(total, fat_bits, root_entries=512, reserved=1, nfats=2):
    root_secs = root_entries * 32 // 512
    for spc in (1, 2, 4, 8, 16, 32, 64):
        fat_size = 1
        while True:                       # iterate until the FAT is big enough
            clusters = (total - reserved - root_secs - nfats * fat_size) // spc
            need = math.ceil((clusters + 2) * fat_bits / 8 / 512)
            if need <= fat_size:
                break
            fat_size = need
        if fat_bits == 12 and clusters < 4085:
            return spc, fat_size, clusters, root_secs
        if fat_bits == 16 and 4085 <= clusters < 65525:
            return spc, fat_size, clusters, root_secs
    sys.exit('mkfat: size does not fit FAT%d' % fat_bits)

class Image:
    def __init__(self, size_kb, fat_bits, label):
        self.total = size_kb * 2
        self.bits = fat_bits
        self.spc, self.fat_size, self.nclusters, self.root_secs = layout(self.total, fat_bits)
        self.csize = self.spc * 512
        self.eoc = 0xFFF if fat_bits == 12 else 0xFFFF
        self.fat = [0] * (self.nclusters + 2)
        self.fat[0] = (0xF00 if fat_bits == 12 else 0xFF00) | 0xF8
        self.fat[1] = self.eoc
        self.next = 2
        self.data = bytearray(self.nclusters * self.csize)
        self.label = label

    def alloc(self, nbytes):
        n = max(1, math.ceil(nbytes / self.csize))
        if self.next + n > self.nclusters + 2:
            sys.exit('mkfat: image too small')
        chain = list(range(self.next, self.next + n))
        self.next += n
        for a, b in zip(chain, chain[1:] + [None]):
            self.fat[a] = b if b else self.eoc
        return chain

    def write_chain(self, chain, payload):
        for i, cl in enumerate(chain):
            chunk = payload[i * self.csize:(i + 1) * self.csize]
            off = (cl - 2) * self.csize
            self.data[off:off + len(chunk)] = chunk

    def build_dir(self, path, my_cluster, parent_cluster):
        """Return the raw directory entries for the directory at path."""
        entries = []
        if my_cluster is not None:       # sub-directories have . and ..
            entries.append(dirent(b'.          ', 0x10, my_cluster, 0))
            entries.append(dirent(b'..         ', 0x10, parent_cluster, 0))
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            if os.path.isdir(full):
                count = 2 + len(os.listdir(full))
                chain = self.alloc(count * 32)
                sub = self.build_dir(full, chain[0], my_cluster or 0)
                self.write_chain(chain, sub)
                entries.append(dirent(name83(name), 0x10, chain[0], 0))
            else:
                content = open(full, 'rb').read()
                cl = 0
                if content:
                    chain = self.alloc(len(content))
                    self.write_chain(chain, content)
                    cl = chain[0]
                entries.append(dirent(name83(name), 0x20, cl, len(content)))
        return b''.join(entries)

    def fat_bytes(self):
        out = bytearray()
        if self.bits == 16:
            for v in self.fat:
                out += struct.pack('<H', v)
        else:
            vals = self.fat + [0]
            for i in range(0, len(self.fat), 2):
                v = vals[i] | (vals[i + 1] << 12)
                out += bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF))
        return bytes(out).ljust(self.fat_size * 512, b'\0')

    def render(self, root_entries_raw):
        if len(root_entries_raw) > self.root_secs * 512:
            sys.exit('mkfat: too many files in the root directory')
        bs = bytearray(512)
        bs[0:3] = b'\xEB\x3C\x90'
        bs[3:11] = b'CLAUDEOS'
        total16 = self.total if self.total < 65536 else 0
        struct.pack_into('<HBHBHHBHHHII', bs, 11, 512, self.spc, 1, 2, 512, total16,
                         0xF8, self.fat_size, 32, 64, 0, self.total if not total16 else 0)
        struct.pack_into('<BBBI11s8s', bs, 36, 0x80, 0, 0x29, 0x20261104,
                         self.label.upper().ljust(11)[:11].encode(),
                         ('FAT12   ' if self.bits == 12 else 'FAT16   ').encode())
        # Tiny real-mode stub: print a message, then INT 18h = "try the next boot device".
        msg = b'ClaudeOS data disk, not bootable. Trying next boot device...\r\n\0'
        code = bytes([0x31, 0xC0, 0x8E, 0xD8, 0xBE, 0x55, 0x7C,   # xor ax,ax; mov ds,ax; mov si,msg
                      0xAC, 0x08, 0xC0, 0x74, 0x06,               # .l: lodsb; or al,al; jz .done
                      0xB4, 0x0E, 0xCD, 0x10, 0xEB, 0xF5,         # mov ah,0Eh; int 10h; jmp .l
                      0xCD, 0x18, 0xF4, 0xEB, 0xFD])              # .done: int 18h; hlt; jmp $-1
        bs[62:62 + len(code)] = code
        bs[85:85 + len(msg)] = msg
        bs[510:512] = b'\x55\xAA'
        fat = self.fat_bytes()
        root = root_entries_raw.ljust(self.root_secs * 512, b'\0')
        img = bytes(bs) + fat + fat + root + bytes(self.data)
        return img.ljust(self.total * 512, b'\0')

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src')
    ap.add_argument('image')
    ap.add_argument('--size-kb', type=int, default=2048)
    ap.add_argument('--fat', type=int, choices=(12, 16))
    ap.add_argument('--label', default='CLAUDEOS')
    a = ap.parse_args()
    bits = a.fat or (12 if a.size_kb <= 2048 else 16)
    img = Image(a.size_kb, bits, a.label)
    root = img.build_dir(a.src, None, None)
    vol = struct.pack('<11sB20x', a.label.upper().ljust(11)[:11].encode(), 0x08)
    data = img.render(vol + root)
    with open(a.image, 'wb') as f:
        f.write(data)
    print('mkfat: %s FAT%d %d KB, %d clusters x %d bytes, %d used' %
          (a.image, bits, a.size_kb, img.nclusters, img.csize, img.next - 2))

if __name__ == '__main__':
    main()
