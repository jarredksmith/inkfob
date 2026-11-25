#!/usr/bin/env python3
"""Builds data/library.bin, the reading library embedded in the pager firmware.

Sources (both cloned from GitHub, see README):
  * ligi/SurvivalManual wiki  - chaptered text and line drawings based on the
    public-domain US Army Survival Manual FM 3-05.70 / FM 21-76
  * everyday3d/esp32-cyd-survival-wiki - 118 short field guides with diagrams
    (MIT licence)

Text is converted to plain ASCII, wrapped to the pager's 40-column small font
and cut into fixed pages, so the firmware only has to copy lines to the
screen. Drawings are converted to 1-bit bitmaps in two sizes: one that fits
the 250-pixel-wide screen and a larger one for zooming.

A drawing gets a page of its own. That page's text is a single line:
    @IMG:<image number>:<caption>

Layout of library.bin, version 3 (little-endian):
  header   : "PGLB", u16 version, u16 books, u16 groups, u16 items,
             u16 images, u16 reserved, u32 pages, u32 stringsSize,
             u32 textSize, u32 imageDataSize                  (32 bytes)
  books    : books  x { u32 titleOffset, u16 firstGroup, u16 groupCount }
  groups   : groups x { u32 titleOffset, u16 firstItem,  u16 itemCount  }
  items    : items  x { u32 titleOffset, u32 firstPage,  u16 pageCount, u16 reserved }
  pageOffs : (pages + 1) x u32   offsets into text
  images   : images x { u32 fitOffset,  u32 fitSize,  u16 fitW,  u16 fitH,
                        u32 zoomOffset, u32 zoomSize, u16 zoomW, u16 zoomH }
             (24 bytes each; zoomW 0 = no zoom version)
  strings  : NUL-terminated titles
  text     : page text, lines separated by '\\n'
  imageData: bitmaps, each a zlib stream. Unpacked it is height rows of
             (width + 7) / 8 bytes, 1 bit per pixel, MSB first, 1 = black.
"""
import re
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageFilter, ImageOps

COLS = 40
LINES_PER_PAGE = 10
TITLE_MAX = 38
SCREEN_W = 250
ZOOM_MAX_W = 500
CAPTION_MAX = 30

# ------------------------------------------------------------------ text

REPLACEMENTS = {
    "‘": "'", "’": "'", "′": "'", "“": '"', "”": '"',
    "–": "-", "—": "-", "‐": "-", "‑": "-", "−": "-",
    "…": "...", " ": " ", " ": " ", " ": " ", "​": "",
    "•": ".", "·": ".", "°": " deg ", "²": "2", "³": "3",
    "½": "1/2", "¼": "1/4", "¾": "3/4", "×": "x", "µ": "u",
    "→": "->", "←": "<-", "≥": ">=", "≤": "<=", "±": "+/-",
    "é": "e", "è": "e", "á": "a", "ñ": "n", "ü": "u", "ö": "o",
    "ä": "a", "ç": "c", "í": "i", "ó": "o", "ú": "u",
    "\u0097": "-", "\u0096": "-", "ﬂ": "fl", "ﬁ": "fi",
}
dropped_chars = {}


def to_ascii(s):
    out = []
    for ch in s:
        if ch in REPLACEMENTS:
            out.append(REPLACEMENTS[ch])
        elif ch == "\t":
            out.append(" ")
        elif ch == "\n" or 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            dropped_chars[ch] = dropped_chars.get(ch, 0) + 1
    return "".join(out)


def strip_inline(s):
    """Removes markdown / HTML decoration from one line of text."""
    s = re.sub(r"<a\s+name=[^>]*>\s*</a>", "", s)
    s = re.sub(r"!\[([^\]]*)\]\([^)]*\)", lambda m: fig_label(m.group(1)), s)
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)        # links
    s = re.sub(r"<br\s*/?>", " ", s)
    s = re.sub(r"</?[a-zA-Z][^>]*>", " ", s)               # other tags
    s = s.replace("\\.", ".").replace("\\-", "-").replace("\\*", "*").replace("\\_", "_")
    s = re.sub(r"\*\*(.+?)\*\*", r"\1", s)
    s = re.sub(r"__(.+?)__", r"\1", s)
    s = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"\1", s)
    s = re.sub(r"(?<![\w_])_(?!\s)(.+?)(?<!\s)_(?![\w_])", r"\1", s)
    s = re.sub(r"`([^`]*)`", r"\1", s)
    s = s.replace("&nbsp;", " ").replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
    return re.sub(r"[ \t]+", " ", s).strip()


def fig_label(alt):
    """Text used for a drawing that could not be included."""
    alt = alt.replace("\\.", ".").strip()
    if not alt:
        return ""
    if alt.lower().startswith("figure"):
        return "[%s - drawing not shown]" % alt
    return "[Drawing not shown: %s]" % alt


def title_case(s):
    small = {"a", "an", "and", "as", "at", "by", "for", "from", "in", "of", "on", "or", "the", "to", "with"}
    words = s.lower().split()
    out = []
    for i, w in enumerate(words):
        if i and w in small:
            out.append(w)
        elif w.upper() in ("CPR", "SOS", "GPS", "FRS", "GMRS", "NOAA", "CNS", "NBC", "II", "III", "ICAO"):
            out.append(w.upper())
        else:
            out.append(w[:1].upper() + w[1:])
    return " ".join(out)


def clip_title(s):
    s = to_ascii(strip_inline(s)).strip(" :-")
    if len(s) > TITLE_MAX:
        s = s[: TITLE_MAX - 3].rstrip() + "..."
    return s or "Untitled"


# ---------------------------------------------------------------- images

class ImageStore:
    """Converts drawings to 1-bit bitmaps and remembers them by source path."""

    def __init__(self):
        self.by_path = {}
        self.images = []      # (fit_bitmap, zoom_bitmap or None), bitmap = (w, h, rows)
        self.missing = []

    def add(self, path, style):
        key = str(path)
        if key in self.by_path:
            return self.by_path[key]
        if not path.exists():
            self.missing.append(key)
            return None
        try:
            im = Image.open(path)
            im.load()
        except Exception as exc:                          # unreadable picture
            self.missing.append("%s (%s)" % (key, exc))
            return None
        if im.mode in ("RGBA", "LA", "P"):
            im = im.convert("RGBA")
            white = Image.new("RGBA", im.size, (255, 255, 255, 255))
            im = Image.alpha_composite(white, im)
        grey = im.convert("L")

        fit_w = min(SCREEN_W, grey.width)
        fit = to_bitmap(scale_to_width(grey, fit_w), style)
        zoom = None
        zoom_w = min(ZOOM_MAX_W, grey.width)
        if zoom_w >= SCREEN_W + 40:                       # only worth it if clearly larger
            zoom = to_bitmap(scale_to_width(grey, zoom_w), style)
        self.images.append((fit, zoom))
        self.by_path[key] = len(self.images) - 1
        return self.by_path[key]


def scale_to_width(grey, width):
    if grey.width == width:
        return grey
    height = max(1, round(grey.height * width / grey.width))
    return grey.resize((width, height), Image.LANCZOS)


def to_bitmap(grey, style):
    """Returns (width, height, rows); each row is bytes, MSB first, 1 = black."""
    w, h = grey.size
    if style == "lineart":
        # Scanned black-on-white line drawings: a plain cut-off is cleanest.
        px = ImageOps.autocontrast(grey, cutoff=1).load()
        black = lambda x, y: px[x, y] <= 170
    else:
        # Shaded diagrams: compare each pixel with its surroundings so that
        # labels stay readable whatever shade they sit on.
        px = grey.load()
        mean = grey.filter(ImageFilter.GaussianBlur(max(4, w // 36))).load()
        black = lambda x, y: px[x, y] < 70 or px[x, y] < mean[x, y] - 14
    stride = (w + 7) // 8
    rows = []
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            if black(x, y):
                row[x >> 3] |= 0x80 >> (x & 7)
        rows.append(bytes(row))
    return (w, h, rows)


def encode_bitmap(bitmap):
    w, h, rows = bitmap
    return zlib.compress(b"".join(rows), 9)


# ------------------------------------------------- markdown -> paragraphs
# A paragraph is (kind, text). kind is 'p', 'h1'..'h6', ('li', marker) for a
# list entry, or ('img', image number) for a drawing (text is its caption).

IMAGE_LINE = re.compile(r"^(?:<a[^>]*>\s*</a>)?\s*!\[([^\]]*)\]\(([^)\s]+)[^)]*\)\s*$")


def table_rows(html):
    rows = []
    for row in re.findall(r"<tr[^>]*>(.*?)</tr>", html, re.S | re.I):
        rows.append(re.findall(r"<t[dh][^>]*>(.*?)</t[dh]>", row, re.S | re.I))
    return rows


def md_to_paragraphs(md, image_dir=None, store=None, style="lineart"):
    md = md.replace("\r\n", "\n")
    paras = []

    # HTML tables: each cell becomes its own block, introduced by its column
    # heading when the table has one, so the columns do not run together.
    def cell_text(cell):
        cell = re.sub(r"<li[^>]*>", "\n* ", cell, flags=re.I)
        cell = re.sub(r"</li>|</?ul[^>]*>|</?ol[^>]*>|<br\s*/?>|</?p[^>]*>", "\n", cell, flags=re.I)
        cell = re.sub(r"(!\[[^\]]*\]\([^)]*\))", r"\n\n\1\n\n", cell)   # pictures on their own line
        return cell.strip()

    def html_table(m):
        html = m.group(0)
        rows = table_rows(html)
        first = re.search(r"<tr[^>]*>(.*?)</tr>", html, re.S | re.I)
        has_header = bool(first and re.search(r"<th", first.group(1), re.I))
        header = [strip_inline(c) for c in rows[0]] if has_header and rows else []
        chunk = []
        for cells in (rows[1:] if has_header else rows):
            for i, cell in enumerate(cells):
                body = cell_text(cell)
                if not body:
                    continue
                if has_header and i < len(header) and header[i]:
                    chunk.append("#### " + header[i])
                chunk.append(body)
        return "\n\n" + "\n\n".join(chunk) + "\n\n"

    md = re.sub(r"<table.*?</table>", html_table, md, flags=re.S | re.I)

    buf = []

    def flush():
        if buf:
            text = strip_inline(" ".join(buf))
            if text:
                paras.append(("p", text))
            buf.clear()

    for raw in md.split("\n"):
        line = raw.rstrip()
        line = re.sub(r"^\s*>\s?", "", line)             # block quotes
        stripped = line.strip()
        if not stripped:
            flush()
            continue
        m = re.match(r"^(#{1,6})\s+(.*)$", stripped)
        if m:
            flush()
            paras.append(("h%d" % len(m.group(1)), strip_inline(m.group(2).strip("# "))))
            continue
        if re.match(r"^\|?\s*:?-{3,}", stripped) and "|" in stripped:
            continue                                      # table separator row
        if stripped.startswith("|") and stripped.endswith("|"):
            flush()
            cells = [strip_inline(c) for c in stripped.strip("|").split("|")]
            paras.append(("p", "  ".join(c for c in cells if c)))
            continue
        m = IMAGE_LINE.match(stripped)
        if m:
            flush()
            alt = m.group(1).replace("\\.", ".").strip()
            index = None
            if store is not None and image_dir is not None and "://" not in m.group(2):
                index = store.add(image_dir / m.group(2), style)
            if index is not None:
                paras.append((("img", index), alt or "Drawing"))
            else:
                text = fig_label(alt)
                if text:
                    paras.append(("p", text))
            continue
        m = re.match(r"^([*+-]|\d{1,2}[.)])\s+(.*)$", stripped)
        if m:
            flush()
            marker = "-" if m.group(1) in "*+-" else m.group(1).replace(")", ".")
            paras.append((("li", marker), strip_inline(m.group(2))))
            continue
        if re.match(r"^(-{3,}|\*{3,}|_{3,})$", stripped):
            flush()
            continue
        buf.append(stripped)
    flush()
    return paras


# ------------------------------------------------------ wrap and paginate

def wrap(text, first_prefix="", rest_prefix=""):
    words = text.split()
    lines, cur, prefix = [], "", first_prefix
    for w in words:
        while len(prefix) + len(w) > COLS:               # very long word / URL
            room = COLS - len(prefix) - (len(cur) + 1 if cur else 0)
            if cur and room < 8:
                lines.append(prefix + cur)
                prefix, cur = rest_prefix, ""
                continue
            piece, w = w[:room], w[room:]
            lines.append(prefix + (cur + " " if cur else "") + piece)
            prefix, cur = rest_prefix, ""
        cand = (cur + " " + w) if cur else w
        if len(prefix) + len(cand) <= COLS:
            cur = cand
        else:
            lines.append(prefix + cur)
            prefix, cur = rest_prefix, w
    if cur:
        lines.append(prefix + cur)
    return lines


def image_line(index, caption):
    caption = to_ascii(caption).replace(":", " -")
    caption = re.sub(r"\s+", " ", caption).strip()
    if len(caption) > CAPTION_MAX:
        caption = caption[: CAPTION_MAX - 3].rstrip() + "..."
    return "@IMG:%d:%s" % (index, caption)


def paragraphs_to_lines(paras):
    """Returns a list of (line, kind) with kind '' (text), 'h' or 'img'."""
    out = []

    def blank():
        if out and out[-1][0] != "" and out[-1][1] != "img":
            out.append(("", ""))

    prev_kind = None
    for kind, text in paras:
        if isinstance(kind, tuple) and kind[0] == "img":
            while out and out[-1][0] == "":
                out.pop()
            out.append((image_line(kind[1], text), "img"))
            prev_kind = kind
            continue
        text = to_ascii(text)
        text = re.sub(r" +", " ", text).strip()
        if not text:
            continue
        if isinstance(kind, tuple):                      # list item
            if not (isinstance(prev_kind, tuple) and prev_kind[0] == "li"):
                blank()
            marker = kind[1] + " "
            for ln in wrap(text, marker, " " * len(marker)):
                out.append((ln, ""))
        elif kind.startswith("h"):
            blank()
            for ln in wrap(text.upper()):
                out.append((ln, "h"))
        else:
            blank()
            for ln in wrap(text):
                out.append((ln, ""))
        prev_kind = kind
    while out and out[-1][0] == "":
        out.pop()
    return out


def paginate(lines):
    pages, cur = [], []

    def close():
        nonlocal cur
        while cur and cur[-1] == "":
            cur.pop()
        if cur:
            pages.append(cur)
        cur = []

    i, n = 0, len(lines)
    while i < n:
        text, kind = lines[i]
        if kind == "img":                                 # a drawing has a page to itself
            close()
            pages.append([text])
            i += 1
            continue
        if not cur and text == "":
            i += 1
            continue                                      # no blank line at top of a page
        if kind == "h":
            # Keep a heading together with at least two lines that follow it.
            j = i
            while j < n and lines[j][1] == "h":
                j += 1
            if cur and len(cur) + (j - i) + 2 > LINES_PER_PAGE:
                close()
                continue
        cur.append(text)
        i += 1
        if len(cur) == LINES_PER_PAGE:
            close()
    close()
    return pages or [["(empty)"]]


# ----------------------------------------------------------------- books

def split_sections(paras, level):
    """Splits paragraphs into (title, paragraphs) at headings of `level`."""
    sections, title, cur = [], None, []
    for kind, text in paras:
        if kind == level:
            if cur:
                sections.append((title or "Overview", cur))
            title, cur = title_case(to_ascii(text)), []
        else:
            cur.append((kind, text))
    if cur:
        sections.append((title or "Overview", cur))
    return sections


def army_manual(wiki_dir, store):
    order = re.findall(r"\[([^\]]+)\]\(([^)]+)\)", (wiki_dir / "Home.md").read_text(encoding="utf-8"))
    skip = {"Apps", "Credits", "FAQ", "TranslatorNotes", "Home"}
    chapters = [(name, page) for name, page in order if page not in skip]
    listed = {page for _, page in chapters}
    extras = [("Ropes and Knots", "RopesAndKnots"), ("Poisonous Plants", "Poisonous-Plants"),
              ("Dangerous Arthropods", "DangerousArthropods"), ("Fish and Mollusks", "FishAndMollusks"),
              ("Self-Defense", "Self-defense")]
    chapters += [(n, p) for n, p in extras if p not in listed and (wiki_dir / (p + ".md")).exists()]

    groups = []
    for name, page in chapters:
        path = wiki_dir / (page + ".md")
        if not path.exists():
            print("  missing chapter file:", page, file=sys.stderr)
            continue
        paras = md_to_paragraphs(path.read_text(encoding="utf-8"), wiki_dir, store, "lineart")
        levels = sorted({k for k, _ in paras if isinstance(k, str) and k.startswith("h")})
        sections = split_sections(paras, levels[0]) if levels else [("Overview", paras)]
        items = [(clip_title(t), paginate(paragraphs_to_lines(p))) for t, p in sections]
        groups.append((clip_title(name), items))
    return ("Army Survival Manual", groups)


def field_guides(wiki_dir, store):
    cats = []
    for line in (wiki_dir / "categories.tsv").read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        cid, name = line.split("\t")[:2]
        cats.append((cid, name))
    by_cat = {cid: [] for cid, _ in cats}
    for path in sorted((wiki_dir / "articles").glob("*.md")):
        text = path.read_text(encoding="utf-8")
        meta = {}
        m = re.match(r"^---\n(.*?)\n---\n(.*)$", text, re.S)
        if m:
            for ln in m.group(1).splitlines():
                if ":" in ln:
                    k, v = ln.split(":", 1)
                    meta[k.strip()] = v.strip()
            text = m.group(2)
        paras = md_to_paragraphs(text, wiki_dir, store, "diagram")
        if meta.get("summary"):
            paras.insert(0, ("p", meta["summary"]))
        title = clip_title(meta.get("title", path.stem))
        order = int(meta.get("order", "99") or 99)
        by_cat.setdefault(meta.get("category", "basics"), []).append(
            (order, title, paginate(paragraphs_to_lines(paras))))
    groups = []
    for cid, name in cats:
        entries = sorted(by_cat.get(cid, []), key=lambda e: (e[0], e[1]))
        if entries:
            groups.append((clip_title(name), [(t, p) for _, t, p in entries]))
    return ("Field Guides", groups)


ABOUT = """This library is stored on the pager and needs no phone or network.

## Army Survival Manual
Based on the US Army Survival Manual, FM 3-05.70 (formerly FM 21-76), a public domain US government work. This edition is the chaptered text and drawings curated by the ligi SurvivalManual project on GitHub, which adds a few chapters of its own.

## Field Guides
118 short guides with diagrams from the esp32-cyd-survival-wiki project by Everyday3D, used under the MIT licence.

## Drawings
A drawing has a page to itself. If it is taller than the screen, the rocker scrolls it before moving on. Press the rocker to zoom in, then MENU to move sideways and EXIT to zoom back out.

## A caution
This is general reference material. It is not a substitute for training, professional medical care or local emergency instructions.
"""


def about_book():
    pages = paginate(paragraphs_to_lines(md_to_paragraphs(ABOUT)))
    return ("About This Library", [("About", [("Sources and Credits", pages)])])


# ------------------------------------------------------------------ pack

def build(books, store, out_path):
    strings = bytearray()

    def add_string(s):
        off = len(strings)
        strings.extend(s.encode("ascii") + b"\0")
        return off

    book_recs, group_recs, item_recs, page_offs = [], [], [], []
    text = bytearray()
    for title, groups in books:
        book_recs.append((add_string(title), len(group_recs), len(groups)))
        for gtitle, items in groups:
            group_recs.append((add_string(gtitle), len(item_recs), len(items)))
            for ititle, pages in items:
                item_recs.append((add_string(ititle), len(page_offs), len(pages)))
                for page in pages:
                    page_offs.append(len(text))
                    text.extend(("\n".join(page)).encode("ascii") + b"\n")
    page_offs.append(len(text))

    image_data = bytearray()
    image_recs = []
    for fit, zoom in store.images:
        fit_off = len(image_data)
        fit_blob = encode_bitmap(fit)
        image_data += fit_blob
        if zoom:
            zoom_off = len(image_data)
            zoom_blob = encode_bitmap(zoom)
            image_data += zoom_blob
            image_recs.append((fit_off, len(fit_blob), fit[0], fit[1], zoom_off, len(zoom_blob), zoom[0], zoom[1]))
        else:
            image_recs.append((fit_off, len(fit_blob), fit[0], fit[1], 0, 0, 0, 0))

    blob = bytearray()
    blob += struct.pack("<4sHHHHHHIIII", b"PGLB", 3, len(book_recs), len(group_recs), len(item_recs),
                        len(image_recs), 0, len(page_offs) - 1, len(strings), len(text), len(image_data))
    for r in book_recs:
        blob += struct.pack("<IHH", *r)
    for r in group_recs:
        blob += struct.pack("<IHH", *r)
    for r in item_recs:
        blob += struct.pack("<IIHH", r[0], r[1], r[2], 0)
    for off in page_offs:
        blob += struct.pack("<I", off)
    for r in image_recs:
        blob += struct.pack("<IIHHIIHH", *r)
    blob += strings
    blob += text
    blob += image_data
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    return dict(books=len(book_recs), groups=len(group_recs), items=len(item_recs),
                pages=len(page_offs) - 1, images=len(image_recs), text=len(text),
                image_data=len(image_data), total=len(blob))


def main():
    ligi_wiki = Path(sys.argv[1])
    cyd_wiki = Path(sys.argv[2])
    out = Path(sys.argv[3])
    store = ImageStore()
    books = [field_guides(cyd_wiki, store), army_manual(ligi_wiki, store), about_book()]
    stats = build(books, store, out)
    for title, groups in books:
        n_items = sum(len(i) for _, i in groups)
        n_pages = sum(len(p) for _, i in groups for _, p in i)
        n_imgs = sum(1 for _, i in groups for _, p in i for pg in p if pg[0].startswith("@IMG:"))
        print("%-24s %3d sections %4d entries %5d pages %4d drawings" % (title, len(groups), n_items, n_pages, n_imgs))
    print(stats)
    if store.missing:
        print("drawings that could not be included (%d):" % len(store.missing))
        for m in store.missing[:20]:
            print("   ", m)
    if dropped_chars:
        print("dropped characters:", {repr(k): v for k, v in sorted(dropped_chars.items(), key=lambda kv: -kv[1])[:20]})


if __name__ == "__main__":
    main()
