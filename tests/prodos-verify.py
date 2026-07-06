#!/usr/bin/env python3
"""Independent ProDOS volume consistency checker.

Walks the volume directory tree of a raw block-ordered ProDOS image
(.po/.hdv) and verifies:
  1. Every allocated block is claimed by exactly ONE owner (boot, voldir,
     bitmap, a directory, or a file's index/data chain). Two owners on one
     block is the shared-block data-loss bug.
  2. Every claimed block is marked USED in the volume bitmap.
  3. No block marked free in the bitmap is claimed.
  4. Bitmap bits at/past total_blocks are all 0 (unavailable).
  5. Directory chains have consistent prev/next pointers.
  6. total_blocks matches the file size (file may be larger; ProDOS ignores
     the excess).
Exit code 0 = clean, 1 = problems found.
"""
import sys

BLK = 512

def main(path):
    data = open(path, 'rb').read()
    if len(data) % BLK:
        print(f"FAIL: file size {len(data)} not a multiple of 512")
        return 1
    fileBlocks = len(data) // BLK
    problems = []
    owners = {}   # block -> owner description

    def blk(n):
        return data[n*BLK:(n+1)*BLK]

    def claim(n, owner):
        if n >= fileBlocks or n < 0:
            problems.append(f"{owner} references block {n} outside the file ({fileBlocks} blocks)")
            return False
        if n in owners:
            problems.append(f"SHARED BLOCK {n}: claimed by both '{owners[n]}' and '{owner}'")
            return False
        owners[n] = owner
        return True

    # --- volume directory header ---
    b2 = blk(2)
    typelen = b2[4]
    if (typelen >> 4) != 0xF:
        print(f"FAIL: block 2 storage type {typelen>>4:#x} is not a volume dir header")
        return 1
    namelen = typelen & 0xF
    volname = b2[5:5+namelen].decode('ascii', 'replace')
    entryLength = b2[0x23]
    entriesPerBlock = b2[0x24]
    fileCount = b2[0x25] | (b2[0x26] << 8)
    bitmapPtr = b2[0x27] | (b2[0x28] << 8)
    totalBlocks = b2[0x29] | (b2[0x2A] << 8)
    print(f"volume '{volname}': {totalBlocks} blocks, bitmap at {bitmapPtr}, "
          f"{fileCount} root entries, entryLength {entryLength:#x}, epb {entriesPerBlock}")
    if entryLength != 0x27: problems.append(f"entryLength {entryLength:#x} != 0x27")
    if entriesPerBlock != 0x0D: problems.append(f"entriesPerBlock {entriesPerBlock} != 13")
    if totalBlocks > fileBlocks:
        problems.append(f"total_blocks {totalBlocks} exceeds file size {fileBlocks} blocks")

    claim(0, "boot block 0")
    claim(1, "boot block 1")

    # --- volume bitmap ---
    bitmapBlocks = (totalBlocks + 4095) // 4096
    for i in range(bitmapBlocks):
        claim(bitmapPtr + i, f"volume bitmap block {i}")
    bm = data[bitmapPtr*BLK:(bitmapPtr+bitmapBlocks)*BLK]
    def bit_free(n):
        return bool(bm[n // 8] & (0x80 >> (n % 8)))
    for n in range(totalBlocks, bitmapBlocks * 4096):
        if n // 8 < len(bm) and bit_free(n):
            problems.append(f"bitmap marks nonexistent block {n} as free")

    # --- walk a directory chain, returning entries ---
    def walk_dir(keyBlock, owner):
        entries = []
        prev = 0
        n = keyBlock
        seen = set()
        while n:
            if n in seen:
                problems.append(f"{owner}: directory chain loops at block {n}")
                break
            seen.add(n)
            if not claim(n, owner):
                break
            b = blk(n)
            p = b[0] | (b[1] << 8)
            nx = b[2] | (b[3] << 8)
            if p != prev:
                problems.append(f"{owner} block {n}: prev pointer {p} != expected {prev}")
            off = 4
            first = (n == keyBlock)
            while off + 0x27 <= 4 + 13*0x27:
                e = b[off:off+0x27]
                st = e[0] >> 4
                nl = e[0] & 0xF
                if not (first and off == 4):  # skip the header entry
                    if st != 0 and nl:
                        entries.append((st, e[1:1+nl].decode('ascii','replace'),
                                        e[0x11] | (e[0x12] << 8),   # key pointer
                                        e[0x15] | (e[0x16] << 8) | (e[0x17] << 16),  # EOF
                                        e[0x10]))                    # file type
                off += 0x27
            prev, n = n, nx
        return entries

    # --- claim a file's index and data blocks ---
    def walk_file(st, name, key, eof, owner):
        # Sparse files store 0 pointers for unallocated blocks; skip those.
        if st == 1:      # seedling: key is the data block
            claim(key, f"{owner} data")
        elif st == 2:    # sapling: key is an index block of up to 256 data ptrs
            if claim(key, f"{owner} index"):
                ib = blk(key)
                for i in range(256):
                    p = ib[i] | (ib[i+256] << 8)
                    if p: claim(p, f"{owner} data[{i}]")
        elif st == 3:    # tree: key is a master index of up to 128 index ptrs
            if claim(key, f"{owner} master index"):
                mb = blk(key)
                for i in range(128):
                    ip = mb[i] | (mb[i+256] << 8)
                    if ip and claim(ip, f"{owner} index[{i}]"):
                        ib = blk(ip)
                        for j in range(256):
                            p = ib[j] | (ib[j+256] << 8)
                            if p: claim(p, f"{owner} data[{i}][{j}]")
        elif st == 0xD:  # subdirectory
            for sub in walk_dir(key, f"dir {owner}"):
                walk_file(sub[0], sub[1], sub[2], sub[3], f"{owner}/{sub[1]}")
        else:
            problems.append(f"{owner}: unhandled storage type {st:#x}")

    for st, name, key, eof, ftype in walk_dir(2, "volume directory"):
        walk_file(st, name, key, eof, name)

    # --- bitmap vs claims ---
    for n in range(totalBlocks):
        free = bit_free(n)
        if n in owners and free:
            problems.append(f"block {n} ('{owners[n]}') is marked FREE in the bitmap")
        if n not in owners and not free:
            problems.append(f"block {n} marked used in bitmap but owned by nothing")

    if problems:
        print(f"FAIL: {len(problems)} problem(s):")
        for p in problems[:40]:
            print(f"  - {p}")
        if len(problems) > 40: print(f"  ... and {len(problems)-40} more")
        return 1
    used = len(owners)
    print(f"OK: {used} blocks allocated, {totalBlocks-used} free, no shared blocks, bitmap consistent")
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
