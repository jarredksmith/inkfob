#!/usr/bin/env python3
"""Reads data/library.bin back independently and checks it against the limits
the firmware relies on.

  check_library.py FILE                      checks only
  check_library.py FILE show [ENTRY ...]     also prints the contents tree and entries
  check_library.py FILE images DIR [N ...]   also writes drawings as PNG files
"""
import struct
import sys
import zlib

COLS, LINES, TITLE_MAX = 40, 10, 38
MAX_BITMAP_BYTES = 32768  # the firmware's unpack buffer

blob = open(sys.argv[1], "rb").read()
HDR = "<4sHHHHHHIIII"
magic, ver, nb, ng, ni, nimg, _, npg, ssz, tsz, isz = struct.unpack_from(HDR, blob, 0)
assert magic == b"PGLB" and ver == 3, (magic, ver)
pos = struct.calcsize(HDR)
assert pos == 32
books = [struct.unpack_from("<IHH", blob, pos + 8 * i) for i in range(nb)]
pos += 8 * nb
groups = [struct.unpack_from("<IHH", blob, pos + 8 * i) for i in range(ng)]
pos += 8 * ng
items = [struct.unpack_from("<IIHH", blob, pos + 12 * i) for i in range(ni)]
pos += 12 * ni
offs = list(struct.unpack_from("<%dI" % (npg + 1), blob, pos))
pos += 4 * (npg + 1)
images = [struct.unpack_from("<IIHHIIHH", blob, pos + 24 * i) for i in range(nimg)]
pos += 24 * nimg
strings = blob[pos:pos + ssz]
text = blob[pos + ssz:pos + ssz + tsz]
image_data = blob[pos + ssz + tsz:pos + ssz + tsz + isz]
assert pos + ssz + tsz + isz == len(blob), "size mismatch"


def title(off):
    end = strings.index(b"\0", off)
    return strings[off:end].decode("ascii")


def page(i):
    return text[offs[i]:offs[i + 1]].decode("ascii").rstrip("\n").split("\n")


def bitmap(index, zoom):
    fo, fl, fw, fh, zo, zl, zw, zh = images[index]
    off, size, w, h = (zo, zl, zw, zh) if zoom else (fo, fl, fw, fh)
    raw = zlib.decompress(image_data[off:off + size])
    return w, h, raw


errors = []
assert offs[0] == 0 and offs[-1] == tsz
used_images = set()
for i in range(npg):
    if offs[i + 1] <= offs[i]:
        errors.append("page %d empty or out of order" % i)
    lines = page(i)
    if len(lines) > LINES:
        errors.append("page %d has %d lines" % (i, len(lines)))
    for ln in lines:
        if len(ln) > COLS:
            errors.append("page %d line too long: %r" % (i, ln))
        if any(ord(c) < 32 or ord(c) > 126 for c in ln):
            errors.append("page %d non-printable: %r" % (i, ln))
    if lines[0].startswith("@IMG:"):
        parts = lines[0].split(":", 2)
        if len(lines) != 1 or len(parts) != 3 or not parts[1].isdigit() or int(parts[1]) >= nimg:
            errors.append("page %d bad drawing line %r" % (i, lines))
        else:
            used_images.add(int(parts[1]))
assert sum(b[2] for b in books) == ng and sum(g[2] for g in groups) == ni
assert sum(it[2] for it in items) == npg
expect = 0
for it in items:
    if it[1] != expect:
        errors.append("item pages not contiguous")
    expect += it[2]
for rec in books + groups + items:
    t = title(rec[0])
    if not t or len(t) > TITLE_MAX:
        errors.append("bad title %r" % t)

checksum = 0
for n in range(nimg):
    for zoom in (False, True):
        if zoom and images[n][6] == 0:
            continue
        try:
            w, h, raw = bitmap(n, zoom)
        except Exception as exc:
            errors.append("drawing %d does not unpack: %s" % (n, exc))
            continue
        if len(raw) != ((w + 7) // 8) * h:
            errors.append("drawing %d wrong size" % n)
        if len(raw) > MAX_BITMAP_BYTES:
            errors.append("drawing %d too big for the firmware buffer: %d" % (n, len(raw)))
        if not zoom and w > 250:
            errors.append("drawing %d fit version wider than the screen" % n)
        checksum = zlib.adler32(raw, checksum)
if used_images != set(range(nimg)):
    errors.append("%d drawings are never shown" % (nimg - len(used_images)))

print("books %d  sections %d  entries %d  pages %d  drawings %d  bytes %d" % (nb, ng, ni, npg, nimg, len(blob)))
print("text lines %d  text chars %d  drawing checksum %08x" % (
    sum(len(page(i)) for i in range(npg)), sum(len(l) for i in range(npg) for l in page(i)), checksum))
print("errors:", len(errors))
for e in errors[:15]:
    print("  ", e)

mode = sys.argv[2] if len(sys.argv) > 2 else ""
if mode == "show":
    for b in books:
        print("\n== %s" % title(b[0]))
        for g in groups[b[1]:b[1] + b[2]]:
            names = [title(it[0]) for it in items[g[1]:g[1] + g[2]]]
            print("  %-30s %3d entries: %s" % (title(g[0]), g[2], "; ".join(names[:4]) + (" ..." if len(names) > 4 else "")))
    for w in sys.argv[3:] or ["Debris Hut"]:
        for it in items:
            if title(it[0]).lower() == w.lower():
                for k in range(min(it[2], 3)):
                    print("\n+" + "-" * COLS + "+  %s  page %d/%d" % (title(it[0]), k + 1, it[2]))
                    for ln in page(it[1] + k):
                        print("|" + ln.ljust(COLS) + "|")
                    print("+" + "-" * COLS + "+")
                break
elif mode == "images":
    from PIL import Image
    out_dir = sys.argv[3]
    wanted = [int(a) for a in sys.argv[4:]] or range(nimg)
    for n in wanted:
        for zoom in (False, True):
            if zoom and images[n][6] == 0:
                continue
            w, h, raw = bitmap(n, zoom)
            im = Image.frombytes("1", (w, h), raw)
            ImageInvertOk = im.point(lambda v: 0 if v else 255)
            ImageInvertOk.save("%s/drawing%03d%s.png" % (out_dir, n, "-zoom" if zoom else ""))
sys.exit(1 if errors else 0)
