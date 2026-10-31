#!/usr/bin/env python3
"""tinyfont2c.py -- convert TinyGo tinyfont Go sources into plain C data arrays.

Reads the Go font definitions shipped by tinygo.org/x/tinyfont (a
`tinyfont.Font` literal whose `Glyphs` slice holds `tinyfont.Glyph` literals) and
emits compact, self-contained C translation units meant to be compiled straight
into an Arduino / ESP-IDF project.  No Go toolchain required.

Layout chosen for flash economy on the ESP32-C3:

  * every glyph bitmap is concatenated into ONE contiguous `uint8_t` blob, so no
    per-glyph array and no pointer per glyph is needed;
  * a glyph descriptor is exactly 12 bytes with no padding:
        rune(4) bitmap_offset(2) width height x_advance bitmap_length(4)
        x_offset y_offset(2)
  * the tables are `const`, i.e. `.rodata`; on ESP32-C3 / ESP-IDF that section
    is already mapped read-only from flash (DROM), so nothing needs RAM and
    there is no AVR-style PROGMEM accessor to write.

The bit packing of the source data is copied verbatim: tinyfont stores W*H bits
MSB-first as one continuous stream (rows are NOT byte aligned), which
`tf_glyph_pixel()` in the generated header reproduces.

Usage:
    python tools/tinyfont2c.py [options] FONT.go [FONT.go ...]

Options:
    --out DIR          output directory (default: c_out)
    --prefix STR       C symbol prefix (default: tf_)
    --name NAME        explicit C symbol base, repeatable, one per source
    --no-umbrella      skip the <prefix>fonts.h umbrella header
    --compile-check    compile the emitted C with gcc, run a host harness and
                       compare all glyphs back against the Go source
    --quiet            less reporting

Exit status is non-zero if a font cannot be converted or a generated file
disagrees with the parsed Go source.

Documented limits of this emitter (all of them are reported, never silent):
  * one glyph bitmap must fit 255 bytes (uint8 bitmap_length); large display
    fonts such as gophers121pt need a wider field;
  * the whole bitmap blob must fit 65535 bytes (uint16 bitmap_offset);
  * const1bit / const2bit fonts are rejected - they are already offset-map
    encoded with palettes and need a different emitter;
  * runes are stored per glyph and looked up by binary search, exactly like
    tinyfont, so a font with large holes costs only 12 bytes per empty slot.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Go source scanning.  Every scan is comment/string aware, so the `/* { */`
# comments tinyfont puts in front of the brace glyphs and bitmap lists that span
# several lines are both handled correctly.
# ---------------------------------------------------------------------------


class GoScanError(Exception):
    pass


def _skip_string(src: str, i: int, quote: str) -> int:
    """Return the index just past the string/char literal starting at `i`."""
    n = len(src)
    j = i + 1
    while j < n:
        c = src[j]
        if quote == "`":
            if c == "`":
                return j + 1
        elif c == "\\":
            j += 2
            continue
        elif c == quote:
            return j + 1
        elif c == "\n":
            raise GoScanError("unterminated string literal")
        j += 1
    raise GoScanError("unterminated string literal")


def _skip_comment(src: str, i: int, end: int) -> int:
    """i points at '/' of a comment; return the index past it (bounded by end)."""
    if src.startswith("//", i):
        j = src.find("\n", i)
        return end if j < 0 else min(j + 1, end)
    j = src.find("*/", i + 2)
    if j < 0:
        raise GoScanError("unterminated block comment")
    return min(j + 2, end)


def match_braces(src: str, open_idx: int) -> int:
    """Index of the `}` that closes the `{` sitting at `open_idx`."""
    if src[open_idx] != "{":
        raise GoScanError("internal: expected '{' at %d" % open_idx)
    depth = 0
    i = open_idx
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and src.startswith("//", i):
            i = _skip_comment(src, i, n)
            continue
        if c == "/" and src.startswith("/*", i):
            i = _skip_comment(src, i, n)
            continue
        if c in "\"`":
            i = _skip_string(src, i, c)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise GoScanError("unbalanced braces starting at %d" % open_idx)


def split_top_level(src: str, sep: str = ",") -> list[str]:
    """Split `src` on `sep` at nesting depth 0, ignoring trivia and literals."""
    out: list[str] = []
    depth = 0
    seg_start = 0
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and (src.startswith("//", i) or src.startswith("/*", i)):
            i = _skip_comment(src, i, n)
            continue
        if c in "\"`":
            i = _skip_string(src, i, c)
            continue
        if c in "{[(":
            depth += 1
        elif c in "}])":
            depth -= 1
        elif c == sep and depth == 0:
            out.append(src[seg_start:i])
            seg_start = i + 1
        i += 1
    out.append(src[seg_start:n])
    return out


def field_map(body: str) -> dict[str, str]:
    """Parse `Key: value, Key: value` pairs out of a composite literal body."""
    fields: dict[str, str] = {}
    for seg in split_top_level(body):
        m = re.match(r"\s*(?:/\*.*?\*/\s*)*([A-Za-z_]\w*)\s*:(.+)", seg, re.S)
        if not m:
            if seg.strip():
                raise GoScanError("unexpected element in glyph literal: %r" % seg.strip()[:60])
            continue
        fields[m.group(1)] = m.group(2).strip()
    return fields


def go_int(text: str) -> int:
    """Parse a Go integer literal: decimal, 0x, 0b, 0o, with optional sign.

    Also accepts the C `u`/`U`/`l`/`L` suffixes, because the verifier feeds this
    the literals it just re-read out of the generated C.
    """
    m = re.fullmatch(r"([+-]?)(0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+|\d+)([uUlL]{0,2})",
                     text.strip())
    if not m:
        raise GoScanError("not an integer literal: %r" % text)
    sign, digits, suffix = m.group(1), m.group(2), m.group(3)
    prefix = digits[:2].lower()
    base = {"0x": 16, "0b": 2, "0o": 8}.get(prefix)
    if base is None:
        # a leading zero means octal only in Go; C literals with a suffix are ours
        base = 8 if (not suffix and digits[0] == "0" and len(digits) > 1) else 10
    value = int(digits, base)
    return -value if sign == "-" else value


def byte_slice(text: str) -> list[int]:
    """Parse `[]uint8{...}` / `[]byte{...}` into a list of byte values."""
    t = text.strip()
    m = re.match(r"\[\](?:uint8|byte)\s*\{", t)
    if not m:
        raise GoScanError("not a byte slice: %r" % t[:60])
    inner = t[m.end() : match_braces(t, m.end() - 1)]
    values = []
    for seg in split_top_level(inner):
        seg = seg.strip()
        if seg:
            values.append(go_int(seg))
    return values


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

GLYPH_KEYS = ("Rune", "Width", "Height", "XAdvance", "XOffset", "YOffset", "Bitmaps")


class Glyph:
    __slots__ = ("rune", "width", "height", "x_advance", "x_offset", "y_offset", "bitmap")

    def __init__(self, rune, width, height, x_advance, x_offset, y_offset, bitmap):
        self.rune, self.width, self.height = rune, width, height
        self.x_advance, self.x_offset, self.y_offset = x_advance, x_offset, y_offset
        self.bitmap = bitmap


class Font:
    __slots__ = ("go_name", "source", "bbox", "y_advance", "glyphs")

    def __init__(self, go_name, source, bbox, y_advance, glyphs):
        self.go_name, self.source, self.bbox = go_name, source, bbox
        self.y_advance, self.glyphs = y_advance, glyphs


FONT_VAR_RE = re.compile(r"\bvar\s+([A-Za-z_]\w*)\s*=\s*((?:[A-Za-z_][\w.]*\.)?Font)\s*\{")
# tinyfont data files come in two spellings: sub-packages write
# `var X = tinyfont.Font{ Glyphs: []tinyfont.Glyph{...} }`, the module-root fonts
# (same package) write `var X = Font{ Glyphs: []Glyph{...} }`.  Field sets are
# identical, so one parser covers both.  const1bit / const2bit fonts use a
# different shape (OffsetMap + Data blob) and are rejected on purpose.
GLYPH_SLICE_RE = re.compile(r"\bGlyphs\s*:\s*\[\s*\]\s*(?:[A-Za-z_][\w.]*\.)?Glyph\s*\{")
GLYPH_LIT_RE = re.compile(r"(?:[A-Za-z_][\w.]*\.)?\bGlyph\s*\{")


def parse_font(path: str) -> Font:
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
        src = fh.read()

    m = FONT_VAR_RE.search(src)
    if not m:
        raise GoScanError("%s: no `var X = tinyfont.Font{...}` declaration" % path)
    name, font_type = m.group(1), m.group(2)
    if "const1bit" in font_type or "const2bit" in font_type:
        raise GoScanError(
            "%s: declares a %s (OffsetMap + packed Data blob, 1/2-bit palettes), which is not "
            "tinyfont.Glyph data - it needs a different emitter" % (path, font_type))
    body = src[m.end() : match_braces(src, m.end() - 1) + 1]

    b_m = re.search(r"\bBBox\s*:\s*\[4\]int8\s*\{", body)
    if not b_m:
        raise GoScanError("%s: missing BBox" % path)
    raw = body[b_m.end() : match_braces(body, b_m.end() - 1)]
    bbox = [go_int(v) for v in split_top_level(raw) if v.strip()]
    if len(bbox) != 4:
        raise GoScanError("%s: BBox needs 4 values, got %d" % (path, len(bbox)))

    y_m = re.search(r"\bYAdvance\s*:\s*(-?0[xX]?[0-9a-fA-F]+|\d+)", body)
    if not y_m:
        raise GoScanError("%s: missing YAdvance (tinyfont's line advance)" % path)
    y_advance = go_int(y_m.group(1))

    g_m = GLYPH_SLICE_RE.search(body)
    if not g_m:
        raise GoScanError("%s: missing Glyphs slice" % path)
    region = body[g_m.end() : match_braces(body, g_m.end() - 1)]

    glyphs: list[Glyph] = []
    for seg in split_top_level(region):
        if not seg.strip():
            continue
        lit = GLYPH_LIT_RE.search(seg)
        if not lit:
            raise GoScanError("%s: Glyphs element is not a Glyph literal: %r"
                              % (path, seg.strip()[:60]))
        inner = seg[lit.end() : match_braces(seg, lit.end() - 1)]
        fields = field_map(inner)
        missing = [k for k in GLYPH_KEYS if k not in fields]
        if missing:
            raise GoScanError("%s: glyph missing field(s): %s" % (path, ", ".join(missing)))
        glyphs.append(Glyph(
            rune=go_int(fields["Rune"]),
            width=go_int(fields["Width"]),
            height=go_int(fields["Height"]),
            x_advance=go_int(fields["XAdvance"]),
            x_offset=go_int(fields["XOffset"]),
            y_offset=go_int(fields["YOffset"]),
            bitmap=byte_slice(fields["Bitmaps"]),
        ))
    if not glyphs:
        raise GoScanError("%s: Glyphs slice is empty" % path)
    return Font(name, os.path.basename(path), bbox, y_advance, glyphs)


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def validate(font: Font, warn: list[str]) -> None:
    for g in font.glyphs:
        where = "%s rune 0x%X" % (font.source, g.rune)
        if not (0 <= g.rune <= 0x10FFFF):
            raise ValueError("%s: outside Unicode range" % where)
        for field in ("width", "height", "x_advance"):
            v = getattr(g, field)
            if not (0 <= v <= 255):
                raise ValueError("%s: %s=%d does not fit uint8" % (where, field, v))
        for field in ("x_offset", "y_offset"):
            v = getattr(g, field)
            if not (-128 <= v <= 127):
                raise ValueError("%s: %s=%d does not fit int8" % (where, field, v))
        for b in g.bitmap:
            if not (0 <= b <= 255):
                raise ValueError("%s: bitmap byte %d out of uint8 range" % (where, b))
        if len(g.bitmap) > 255:
            raise ValueError(
                "%s: %d-byte bitmap does not fit uint8 bitmap_length - this emitter targets "
                "small UI fonts; widen tf_glyph_t.bitmap_length to uint16_t in the shared header "
                "for fonts like gophers121pt (costs 2 bytes per glyph)" % (where, len(g.bitmap)))
        need = (g.width * g.height + 7) // 8
        if len(g.bitmap) != need:
            warn.append("%s: bitmap is %d byte(s) but ceil(w*h/8) = %d "
                        "(stored as-is; tf_glyph_pixel() bounds-checks reads)"
                        % (where, len(g.bitmap), need))
        if g.width == 0 and g.height > 0:
            warn.append("%s: height %d with zero width - nothing can be drawn" % (where, g.height))

    for a, b in zip(font.glyphs, font.glyphs[1:]):
        if a.rune == b.rune:
            raise ValueError("%s: duplicate rune 0x%X (tinyfont's binary search would be ambiguous)"
                             % (font.source, a.rune))
    if any(b.rune < a.rune for a, b in zip(font.glyphs, font.glyphs[1:])):
        warn.append("%s: glyphs were not in ascending rune order; the emitted C table is sorted "
                    "so tf_font_find() works (the Go binary search requires the same order)"
                    % font.source)
        font.glyphs.sort(key=lambda g: g.rune)

    for i, v in enumerate(font.bbox):
        if not (-128 <= v <= 127):
            raise ValueError("%s: BBox[%d]=%d does not fit int8" % (font.source, i, v))
    if not (0 <= font.y_advance <= 255):
        raise ValueError("%s: YAdvance=%d does not fit uint8" % (font.source, font.y_advance))
    if not (1 <= len(font.glyphs) <= 65535):
        raise ValueError("%s: glyph count %d does not fit uint16" % (font.source, len(font.glyphs)))
    if sum(len(g.bitmap) for g in font.glyphs) > 65535:
        raise ValueError("%s: bitmap blob would exceed 65535 bytes; widen tf_glyph_t.bitmap_offset "
                         "to uint32_t in the shared header (costs 4 bytes per glyph)" % font.source)


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def c_ident(name: str) -> str:
    return re.sub(r"[^0-9a-zA-Z_]", "_", name)


def glyph_comment(rune: int) -> str:
    """ASCII-only hint after the row.  Costs source bytes, never flash."""
    if 0x20 < rune < 0x7F and chr(rune) not in ("'", "\\"):
        return "  /* '%s' */" % chr(rune)
    return ""


def emit_bitmaps(blob: list[int], per_line: int = 16) -> list[str]:
    return ["    %s" % " ".join("0x%02X," % b for b in blob[i : i + per_line])
            for i in range(0, len(blob), per_line)]


def build_blob(font: Font) -> tuple[list[int], list[int]]:
    blob: list[int] = []
    offsets: list[int] = []
    for g in font.glyphs:
        offsets.append(len(blob))
        blob.extend(g.bitmap)
    return blob, offsets


def glyph_rows(font: Font, offsets: list[int]) -> list[str]:
    """One glyph per line, data only: the caller adds the separator and hint."""
    return [
        "    { 0x%08X, %5u, %3u, %3u, %3u, %3u, %4d, %4d }"
        % (g.rune, ofs, g.width, g.height, g.x_advance, len(g.bitmap), g.x_offset, g.y_offset)
        for g, ofs in zip(font.glyphs, offsets)
    ]


def emit_font_c(font: Font, base: str, c_name: str, prefix: str) -> tuple[str, int]:
    blob, offsets = build_blob(font)
    symbol = prefix + base
    L: list[str] = []
    A = L.append
    A("/* %s - generated by tools/tinyfont2c.py." % c_name)
    A(" *")
    A(" * Source: tinygo.org/x/tinyfont  %s  (`var %s tinyfont.Font`)" % (font.source, font.go_name))
    A(" * %d glyphs, bitmap blob %d bytes, glyph table %d bytes, total %d bytes of flash"
      % (len(font.glyphs), len(blob), 12 * len(font.glyphs), len(blob) + 12 * len(font.glyphs)))
    A(" * (tf_font_t itself is 16 bytes on the 32-bit ESP32-C3 ABI).")
    A(" *")
    A(" * Do not edit by hand - rerun the generator instead.")
    A(" */")
    A("")
    A('#include "%s"' % (c_name[: -len(".c")] + ".h"))
    A("")
    A("/* All glyph bitmaps, concatenated in glyph order.  Each glyph records its own")
    A(" * start offset and byte count in tf_glyph_t, so the font needs exactly one")
    A(" * array here: no per-glyph arrays and no pointers.  Bits are packed MSB-first")
    A(" * as a single continuous stream per glyph, so rows are NOT byte aligned. */")
    A("static const uint8_t %s_bitmaps_[%s] = {" % (symbol, "%du" % len(blob) if blob else "1u"))
    blob_lines = emit_bitmaps(blob) or ["    0x00"]
    if blob:
        blob_lines[-1] = blob_lines[-1].rstrip(",")  # no trailing comma: C89-clean too
    L.extend(blob_lines)
    A("};")
    A("")
    A("/* Sorted by ascending rune, which is what tf_font_find()'s binary search needs.")
    A(" *    column order: rune, bitmap_offset, width, height, x_advance,")
    A(" *                  bitmap_length, x_offset, y_offset */")
    A("static const tf_glyph_t %s_glyphs_[%d] = {" % (symbol, len(font.glyphs)))
    rows = glyph_rows(font, offsets)
    for i, (row, g) in enumerate(zip(rows, font.glyphs)):
        A(row + ("," if i + 1 < len(rows) else "") + glyph_comment(g.rune))
    A("};")
    A("")
    record = [
        ("{ %d, %d, %d, %d }," % tuple(font.bbox), "bbox: max width, max height, min x, min y"),
        ("%d," % len(font.glyphs), "glyph_count"),
        ("%d," % font.y_advance, "y_advance"),
        ("%s_glyphs_," % symbol, "glyphs"),
        ("%s_bitmaps_" % symbol, "bitmaps"),
    ]
    col = max(len(text) for text, _ in record)
    A("const tf_font_t %s = {" % symbol)
    for text, comment in record:
        A("    %-*s  /* %s */" % (col, text, comment))
    A("};")
    A("")
    return "\n".join(L), len(blob)


def emit_font_h(font: Font, base: str, header_name: str, prefix: str, shared_header: str) -> str:
    guard = c_ident(header_name.upper().replace(".", "_"))
    symbol = prefix + base
    L: list[str] = []
    A = L.append
    A("/* %s - generated by tools/tinyfont2c.py. Do not edit by hand." % header_name)
    A(" *")
    A(" * Source: tinygo.org/x/tinyfont %s (`var %s`)" % (font.source, font.go_name))
    A(" */")
    A("#ifndef %s" % guard)
    A("#define %s" % guard)
    A("")
    A('#include "%s"' % shared_header)
    A("")
    A("#ifdef __cplusplus")
    A('extern "C" {')
    A("#endif")
    A("")
    A("#define %s_GLYPH_COUNT %du" % (c_ident(symbol).upper(), len(font.glyphs)))
    A("")
    A("/* %s: %d glyphs, y_advance %d, bbox [%d, %d, %d, %d]. */"
      % (symbol, len(font.glyphs), font.y_advance, font.bbox[0], font.bbox[1], font.bbox[2], font.bbox[3]))
    A("extern const tf_font_t %s;" % symbol)
    A("")
    A("#ifdef __cplusplus")
    A("}")
    A("#endif")
    A("")
    A("#endif /* %s */" % guard)
    A("")
    return "\n".join(L)


SHARED_FOOTER = '''

/* ---------------------------------------------------------------------------
 * Header-only helpers: bit unpacking and lookup.  No allocation, no recursion,
 * no floating point - cheap enough to call per pixel from a frame loop.
 * ------------------------------------------------------------------------ */

/* Pixel (x, y) of a glyph, 1 = ink, 0 = background.
 *
 * tinyfont packs width*height bits MSB-first into one continuous stream, hence
 * the bit index y*width + x: rows are NOT byte aligned and there is no stride
 * to precompute.  Reads past the stored bytes return 0. */
static inline int tf_glyph_pixel(const tf_font_t *f, const tf_glyph_t *g,
                                 unsigned int x, unsigned int y)
{
    unsigned int bit;

    if (g == NULL || f == NULL || f->bitmaps == NULL) {
        return 0;
    }
    if (x >= (unsigned int)g->width || y >= (unsigned int)g->height) {
        return 0;
    }
    bit = y * (unsigned int)g->width + x;
    if ((bit >> 3u) >= (unsigned int)g->bitmap_length) {
        return 0;
    }
    return (int)((f->bitmaps[g->bitmap_offset + (bit >> 3u)] >> (7u - (bit & 7u))) & 1u);
}

/* Binary search, mirroring tinyfont's Font.GetGlyph().  Returns NULL for a rune
 * the font does not contain. */
static inline const tf_glyph_t *tf_font_find(const tf_font_t *f, uint32_t rune)
{
    unsigned int lo = 0u;
    unsigned int hi = (unsigned int)f->glyph_count;

    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2u;
        if (f->glyphs[mid].rune < rune) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if ((lo < (unsigned int)f->glyph_count) && (f->glyphs[lo].rune == rune)) {
        return &f->glyphs[lo];
    }
    return NULL;
}

/* tinyfont synthesises an empty glyph for a missing rune that still carries the
 * x_advance / x_offset / y_offset of the font's FIRST glyph, which is what keeps
 * the pen moving over unencoded characters.  Replicate that here: width and
 * height are 0, so nothing is drawn. */
static inline void tf_font_missing_glyph(const tf_font_t *f, tf_glyph_t *out)
{
    out->rune = 0u;
    out->width = 0u;
    out->height = 0u;
    out->bitmap_offset = 0u;
    out->bitmap_length = 0u;
    if (f->glyph_count > 0u) {
        out->x_advance = f->glyphs[0].x_advance;
        out->x_offset = f->glyphs[0].x_offset;
        out->y_offset = f->glyphs[0].y_offset;
    } else {
        out->x_advance = 0u;
        out->x_offset = 0;
        out->y_offset = 0;
    }
}

/* Pen advance for a rune, using tinyfont's fallback for a missing glyph. */
static inline unsigned int tf_font_advance(const tf_font_t *f, uint32_t rune)
{
    const tf_glyph_t *g = tf_font_find(f, rune);

    if (g != NULL) {
        return (unsigned int)g->x_advance;
    }
    if (f->glyph_count > 0u) {
        return (unsigned int)f->glyphs[0].x_advance;
    }
    return 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* %(guard)s */
'''


def emit_shared_header(prefix: str, header_name: str) -> str:
    guard = c_ident(header_name.upper().replace(".", "_"))
    L: list[str] = []
    A = L.append
    A("/* %s - generated by tools/tinyfont2c.py. Do not edit by hand." % header_name)
    A(" *")
    A(" * Plain-C data model for bitmaps converted from tinygo.org/x/tinyfont.")
    A(" *")
    A(" * Flash placement: every generated table is `const`, so it lands in .rodata.")
    A(" * On ESP32-C3 / ESP-IDF that section is mapped read-only from flash (DROM) and")
    A(" * can be indexed directly - there is no AVR-style PROGMEM and no pgm_read_*")
    A(" * accessor to write.  Nothing in this header needs RAM.")
    A(" */")
    A("#ifndef %s" % guard)
    A("#define %s" % guard)
    A("")
    A("#include <stdint.h>")
    A("#include <stddef.h>")
    A("")
    A("#ifdef __cplusplus")
    A('extern "C" {')
    A("#endif")
    A("")
    A("/* One glyph: metrics plus where its bitmap sits in the font blob.")
    A(" * Field order packs into exactly 12 bytes on 16/32/64-bit targets, which the")
    A(" * compile-time assertion below checks. */")
    A("typedef struct tf_glyph_s {")
    A("    uint32_t rune;           /* code point                        */")
    A("    uint16_t bitmap_offset;  /* index into tf_font_t::bitmaps     */")
    A("    uint8_t  width;          /* bitmap box, in pixels             */")
    A("    uint8_t  height;")
    A("    uint8_t  x_advance;      /* pen advance, in pixels            */")
    A("    uint8_t  bitmap_length;  /* bytes at bitmap_offset            */")
    A("    int8_t   x_offset;       /* box left edge relative to the pen */")
    A("    int8_t   y_offset;       /* box top relative to the baseline;")
    A("                              *   negative is above the baseline  */")
    A("} tf_glyph_t;")
    A("")
    A("/* A font: global metrics plus the two tables the generator emits. */")
    A("typedef struct tf_font_s {")
    A("    int8_t            bbox[4];      /* max width, max height, min x, min y */")
    A("    uint16_t          glyph_count;")
    A("    uint8_t           y_advance;    /* line advance, in pixels             */")
    A("    const tf_glyph_t *glyphs;       /* glyph_count entries, sorted by rune */")
    A("    const uint8_t    *bitmaps;      /* packed bitmap blob                  */")
    A("} tf_font_t;")
    A("")
    A("typedef char tf_glyph_size_check[(sizeof(tf_glyph_t) == 12u) ? 1 : -1];")
    A("typedef char tf_font_field_check[(sizeof(((tf_font_t *)0)->bbox) == 4u) ? 1 : -1];")
    return "\n".join(L) + SHARED_FOOTER % {"guard": guard}


def emit_umbrella(header_name: str, font_headers: list[str]) -> str:
    guard = c_ident(header_name.upper().replace(".", "_"))
    L = [
        "/* %s - generated by tools/tinyfont2c.py. Do not edit by hand." % header_name,
        " *",
        " * Includes every converted font, so a single include covers the theme.",
        " */",
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "",
    ]
    L += ['#include "%s"' % h for h in font_headers]
    L += ["", "#endif /* %s */" % guard, ""]
    return "\n".join(L)


# ---------------------------------------------------------------------------
# Verification #1: re-read the emitted C text and compare it to the Go model
# ---------------------------------------------------------------------------

C_BYTE = re.compile(r"^0[xX][0-9a-fA-F]{1,2}$")
C_ROW = re.compile(
    r"^\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,"
    r"\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}$"
)


def c_array_body(text: str, decl: str) -> str:
    """Body of `{ ... }` that follows `decl` in emitted C (brace + comment aware)."""
    m = re.search(decl + r"\s*\{", text)
    if not m:
        raise AssertionError("generated C has no %s initializer" % decl)
    open_idx = text.index("{", m.end() - 1)
    return text[open_idx + 1 : match_braces(text, open_idx)]


def verify_written(font: Font, symbol: str, c_text: str) -> None:
    """Parse the emitted initializer lists back and compare them to the Go model.

    Deliberately re-parses with a brace/comma aware splitter instead of loose
    regexes, so a formatting slip (a missing comma, a merged row) fails here as
    well as in the compiler.
    """
    blob = []
    for elem in split_top_level(c_array_body(c_text, r"_bitmaps_\[\d+u?\]\s*=")):
        elem = elem.strip()
        if not elem:
            continue
        if not C_BYTE.match(elem):
            raise AssertionError("bitmap initializer element is not a single byte: %r" % elem[:40])
        blob.append(go_int(elem))

    rows = []
    for elem in split_top_level(c_array_body(c_text, r"_glyphs_\[\d+\]\s*=")):
        elem = elem.strip()
        if not elem:
            continue
        m = C_ROW.match(re.sub(r"/\*.*?\*/", "", elem).strip().rstrip(",").strip())
        if not m:
            raise AssertionError("glyph initializer element is malformed: %r" % elem[:60])
        rows.append(m.groups())

    if len(rows) != len(font.glyphs):
        raise AssertionError("re-parsed %d glyph rows, expected %d" % (len(rows), len(font.glyphs)))
    expect = [b for g in font.glyphs for b in g.bitmap]
    if blob != expect:
        raise AssertionError("bitmap blob round-trip mismatch (%d bytes, expected %d)"
                             % (len(blob), len(expect)))

    cur = 0
    for g, r in zip(font.glyphs, rows):
        rune, ofs, w, h, adv, blen, xo, yo = (int(v, 0) for v in r)
        if (rune, w, h, adv, xo, yo) != (g.rune, g.width, g.height, g.x_advance, g.x_offset, g.y_offset):
            raise AssertionError("glyph metrics round-trip mismatch for rune 0x%X" % g.rune)
        if ofs != cur:
            raise AssertionError("rune 0x%X starts at %d, expected %d" % (g.rune, ofs, cur))
        if blob[ofs : ofs + blen] != g.bitmap:
            raise AssertionError("glyph bitmap round-trip mismatch for rune 0x%X" % g.rune)
        cur += blen
    if cur != len(expect):
        raise AssertionError("blob not exactly tiled: %d != %d" % (cur, len(expect)))

    # the tf_font_t record itself
    m = re.search(r"const tf_font_t %s = \{" % re.escape(symbol), c_text)
    if not m:
        raise AssertionError("generated C does not define `const tf_font_t %s`" % symbol)
    body = c_array_body(c_text, r"const tf_font_t %s\s*=" % re.escape(symbol))
    elems = [re.sub(r"/\*.*?\*/", "", e).strip() for e in split_top_level(body) if e.strip()]
    bbox = [int(v) for v in re.findall(r"-?\d+", elems[0])]
    if bbox != font.bbox:
        raise AssertionError("bbox in generated C is %s, expected %s" % (bbox, font.bbox))
    if int(elems[1]) != len(font.glyphs):
        raise AssertionError("glyph_count in generated C is %s" % elems[1])
    if go_int(elems[2]) != font.y_advance:
        raise AssertionError("y_advance in generated C is %s, expected %d" % (elems[2], font.y_advance))


# ---------------------------------------------------------------------------
# Verification #2: compile the emitted C and compare every glyph at run time
# ---------------------------------------------------------------------------

HARNESS = r"""/* Host round-trip harness written by tinyfont2c.py (compile-check only).
 * Prints one GL line per glyph so the C tables can be compared against the Go
 * source model, then exits non-zero if a lookup disagrees with the table. */
#include <stdio.h>
#include <stdint.h>
@INCLUDES@

static const tf_font_t *const fonts[] = { @FONTS@ };
static const char *const tags[] = { @TAGS@ };

int main(void)
{
    unsigned int i, k;

    printf("SZG %u\n", (unsigned)sizeof(tf_glyph_t));
    printf("SZF %u\n", (unsigned)sizeof(tf_font_t));
    for (i = 0u; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        const tf_font_t *f = fonts[i];
        tf_glyph_t fb;
        for (k = 0u; k < f->glyph_count; ++k) {
            const tf_glyph_t *g = &f->glyphs[k];
            unsigned long sum = 0ul;
            unsigned int ink = 0u, x, y, b;
            for (b = 0u; b < g->bitmap_length; ++b) {
                sum += f->bitmaps[g->bitmap_offset + b];
            }
            for (y = 0u; y < g->height; ++y) {
                for (x = 0u; x < g->width; ++x) {
                    ink += (unsigned)tf_glyph_pixel(f, g, x, y);
                }
            }
            printf("GL %s %u %u %u %u %d %d %u %lu %u\n", tags[i], (unsigned)g->rune,
                   (unsigned)g->width, (unsigned)g->height, (unsigned)g->x_advance,
                   (int)g->x_offset, (int)g->y_offset, (unsigned)g->bitmap_length, sum, ink);
            if (tf_font_find(f, g->rune) != g) {
                (void)printf("LOOKUP_FAIL %u\n", (unsigned)g->rune);
                return 2;
            }
        }
        if (tf_font_find(f, 0xFFFFFFFFu) != NULL) {
            (void)printf("LOOKUP_GHOST\n");
            return 3;
        }
        tf_font_missing_glyph(f, &fb);
        printf("META %s %d %d %d %d %u %u %u %d %d\n", tags[i], (int)f->bbox[0], (int)f->bbox[1],
               (int)f->bbox[2], (int)f->bbox[3], (unsigned)f->y_advance, (unsigned)f->glyph_count,
               (unsigned)fb.x_advance, (int)fb.x_offset, (int)fb.y_offset);
    }
    return 0;
}
"""


def _run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True)


def compile_check(fonts: list[tuple[Font, str]], out_dir: str, prefix: str, shared_header: str) -> None:
    if shutil.which("gcc") is None:
        print("  [skip] gcc not found on PATH, cannot compile-check")
        return

    symbols = [prefix + base for _f, base in fonts]
    c_units = [os.path.join(out_dir, "%s.c" % s) for s in symbols]
    harness_src = (
        HARNESS.replace("@INCLUDES@",
                        '#include "%s"\n' % shared_header
                        + "\n".join('#include "%s.h"\n' % s for s in symbols))
        .replace("@FONTS@", ", ".join("&%s" % s for s in symbols))
        .replace("@TAGS@", ", ".join('"%s"' % s for s in symbols))
    )

    tmp = tempfile.mkdtemp(prefix="tinyfont2c_")
    try:
        harness = os.path.join(tmp, "harness.c")
        with open(harness, "w", encoding="ascii") as fh:
            fh.write(harness_src)

        exe = os.path.join(tmp, "harness.exe" if os.name == "nt" else "harness")
        cc = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O1",
              "-I", out_dir, harness] + c_units + ["-o", exe]
        r = _run(cc)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            raise AssertionError("gcc rejected the generated C")
        print("  [ok] gcc -std=c11 -Wall -Wextra -Werror -pedantic: %d font unit(s) compile clean"
              % len(c_units))

        # Arduino sketches are compiled as C++, so the headers have to survive that too.
        if shutil.which("g++"):
            cpp = os.path.join(tmp, "cpp_include.cpp")
            with open(cpp, "w", encoding="ascii") as fh:
                fh.write("\n".join(['#include "%s.h"' % s for s in symbols])
                         + "\nunsigned int probe(const tf_font_t *f, unsigned int r) {"
                         + "\n    const tf_glyph_t *g = tf_font_find(f, r);"
                         + "\n    return (g != NULL) ? (unsigned int)g->rune : tf_font_advance(f, r);"
                         + "\n}\n")
            rc = _run(["g++", "-x", "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                       "-I", out_dir, "-c", cpp, "-o", os.path.join(tmp, "cpp_include.o")])
            print("  [%s] the headers also compile as C++ (Arduino .ino is compiled with g++)"
                  % ("ok" if rc.returncode == 0 else "FAIL",))
            if rc.returncode != 0:
                print(rc.stdout + rc.stderr)
                raise AssertionError("g++ rejected the generated headers")
        else:
            print("  [skip] g++ not found (no C++ include check)")

        # ILP32 layout probe: on the ESP32-C3 (RISC-V, 32-bit int and pointer) a
        # glyph record must be 12 bytes and the font record 16 bytes.
        arm = shutil.which("arm-none-eabi-gcc")
        probe = os.path.join(tmp, "size_probe.c")
        with open(probe, "w", encoding="ascii") as fh:
            fh.write(chr(10).join([
                '#include "%s"' % shared_header,
                'typedef char glyph_is_12_bytes[(sizeof(tf_glyph_t) == 12u) ? 1 : -1];',
                'typedef char font_record_is_16_bytes[(sizeof(tf_font_t) == 16u) ? 1 : -1];',
                'typedef char ilp32_abi[(sizeof(void *) == 4u && sizeof(int) == 4u) ? 1 : -1];',
                '',
            ]))
        compilers = [["gcc", "-m32"]] + ([[arm]] if arm else [])
        probed = None
        for cc32 in compilers:
            r32 = _run(cc32 + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Os",
                               "-I", out_dir, "-c", probe, "-o", os.path.join(tmp, "probe32.o")])
            if r32.returncode == 0:
                probed = " ".join(os.path.basename(c) for c in cc32)
                break
        print("  [%s] ILP32 layout: 12 B per glyph record, 16 B per font record (%s)"
              % ("ok" if probed else "skip", probed or "no 32-bit compiler available"))

        if arm:
            ok = True
            for unit in c_units:
                rr = _run([arm, "-mcpu=cortex-m4", "-mthumb", "-std=c11", "-Wall", "-Wextra",
                           "-Werror", "-Os", "-ffunction-sections", "-I", out_dir, "-c", unit,
                           "-o", os.path.join(tmp, "%s.o" % os.path.basename(unit))])
                if rr.returncode != 0:
                    ok = False
                    print(rr.stdout + rr.stderr)
            print("  [%s] arm-none-eabi-gcc -Os -std=c11 -Wall -Wextra -Werror compiles the data units"
                  % ("ok" if ok else "FAIL",))
            if not ok:
                raise AssertionError("arm-none-eabi-gcc rejected the generated C")
        else:
            print("  [skip] arm-none-eabi-gcc not found (no 32-bit cross compile check)")

        run = _run([exe])
        if run.returncode != 0:
            raise AssertionError("harness exited %d\n%s%s" % (run.returncode, run.stdout, run.stderr))

        lines = run.stdout.splitlines()
        if len(lines) < 2 or not lines[0].startswith("SZG "):
            raise AssertionError("harness produced no output")
        sz_glyph, sz_font = int(lines[0].split()[1]), int(lines[1].split()[1])
        if sz_glyph != 12:
            raise AssertionError("sizeof(tf_glyph_t) == %d, expected 12" % sz_glyph)
        print("  [ok] sizeof(tf_glyph_t)=%d sizeof(tf_font_t)=%d" % (sz_glyph, sz_font))

        rows: dict[str, list[list[str]]] = {}
        meta: dict[str, list[str]] = {}
        for line in lines[2:]:
            parts = line.split()
            if parts[0] == "GL":
                rows.setdefault(parts[1], []).append(parts[2:])
            elif parts[0] == "META":
                meta[parts[1]] = parts[2:]

        total = 0
        for font, base in fonts:
            tag = prefix + base
            got = rows.get(tag)
            if got is None or len(got) != len(font.glyphs):
                raise AssertionError("%s: harness returned %s rows, expected %d"
                                     % (tag, "no" if got is None else len(got), len(font.glyphs)))
            for g, p in zip(font.glyphs, got):
                rune, w, h, adv, xo, yo, blen, sum_, ink = (int(v) for v in p)
                if rune != g.rune:
                    raise AssertionError("%s: harness rune %d != model 0x%X" % (tag, rune, g.rune))
                if (w, h, adv, xo, yo, blen) != (g.width, g.height, g.x_advance, g.x_offset,
                                                 g.y_offset, len(g.bitmap)):
                    raise AssertionError("%s rune 0x%X: metrics disagree with the Go source"
                                         % (tag, g.rune))
                if sum_ != sum(g.bitmap):
                    raise AssertionError("%s rune 0x%X: bitmap bytes disagree" % (tag, g.rune))
                # independent ground truth: unpack the bits exactly the way tinyfont's
                # Glyph.Draw() walks them and count the lit pixels
                expect_ink = 0
                for y in range(g.height):
                    for x in range(g.width):
                        idx = y * g.width + x
                        if (idx >> 3) < len(g.bitmap):
                            expect_ink += (g.bitmap[idx >> 3] >> (7 - (idx & 7))) & 1
                if ink != expect_ink:
                    raise AssertionError("%s rune 0x%X: ink %d != model %d" % (tag, ink, expect_ink))
                total += 1
            m = meta.get(tag)
            if m is None:
                raise AssertionError("%s: harness reported no META line" % tag)
            if [int(v) for v in m[:4]] != font.bbox:
                raise AssertionError("%s: bbox %s != %s" % (tag, m[:4], font.bbox))
            if int(m[4]) != font.y_advance or int(m[5]) != len(font.glyphs):
                raise AssertionError("%s: y_advance / glyph_count disagree" % tag)
            first = font.glyphs[0]
            if (int(m[6]), int(m[7]), int(m[8])) != (first.x_advance, first.x_offset, first.y_offset):
                raise AssertionError("%s: missing-glyph fallback should mirror glyph[0] metrics" % tag)
        print("  [ok] %d glyphs read back through the C arrays and tf_glyph_pixel() match the Go source"
              % total)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Convert tinyfont Go sources to plain C arrays.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="*", help="Go files holding a tinyfont.Font literal")
    ap.add_argument("--out", default="c_out", help="output directory (default: c_out)")
    ap.add_argument("--prefix", default="tf_", help="C symbol prefix (default: tf_)")
    ap.add_argument("--name", action="append", default=[], dest="names",
                    help="explicit C symbol base, repeatable, one per source")
    ap.add_argument("--no-umbrella", action="store_true", help="skip the tf_fonts.h umbrella header")
    ap.add_argument("--compile-check", action="store_true",
                    help="compile the emitted C with gcc and verify every glyph round-trips")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if not args.sources:
        ap.error("at least one Go font source is required")
    if args.names and len(args.names) != len(args.sources):
        ap.error("--name must be given once per source (%d given, %d sources)"
                 % (len(args.names), len(args.sources)))
    prefix = args.prefix
    if prefix and not re.fullmatch(r"[A-Za-z_][0-9a-zA-Z_]*", prefix):
        ap.error("--prefix must be a C identifier fragment, got %r" % prefix)

    stem = prefix if (not prefix or prefix.endswith("_")) else prefix + "_"
    shared_header = "%sfont.h" % stem
    umbrella = "%sfonts.h" % stem

    warn: list[str] = []
    parsed: list[tuple[Font, str]] = []
    stem_lower = stem.lower()
    seen: dict[str, str] = {}
    for i, src in enumerate(args.sources):
        font = parse_font(src)
        base = c_ident(args.names[i] if args.names else font.go_name.lower())
        # a --name that already spells out the prefix must not double it
        if base.startswith(stem_lower) and len(base) > len(stem_lower):
            base = base[len(stem) :].lstrip("_")
        if not base:
            ap.error("source %s collapses to an empty C symbol; pass --name" % src)
        if base in seen:
            ap.error("symbol %s%s is produced by both %s and %s; disambiguate with --name"
                     % (prefix, base, seen[base], src))
        seen[base] = src
        validate(font, warn)
        parsed.append((font, base))

    os.makedirs(args.out, exist_ok=True)
    written = []

    def write(name: str, text: str) -> None:
        path = os.path.join(args.out, name)
        with open(path, "w", encoding="ascii", newline="\n") as fh:
            fh.write(text)
        written.append(path)

    write(shared_header, emit_shared_header(prefix, shared_header))

    font_headers = []
    total_bytes = 0
    for font, base in parsed:
        header_name = "%s%s.h" % (stem, base)
        c_name = "%s%s.c" % (stem, base)
        c_text, blob_len = emit_font_c(font, base, c_name, prefix)
        verify_written(font, prefix + base, c_text)
        write(c_name, c_text)
        write(header_name, emit_font_h(font, base, header_name, prefix, shared_header))
        font_headers.append(header_name)
        total_bytes += blob_len + 12 * len(font.glyphs)
        if not args.quiet:
            print("%-26s %3d glyphs  blob %5d B + table %5d B = %5d B flash"
                  % (prefix + base, len(font.glyphs), blob_len, 12 * len(font.glyphs),
                     blob_len + 12 * len(font.glyphs)))

    if not args.no_umbrella:
        write(umbrella, emit_umbrella(umbrella, font_headers))

    if not args.quiet:
        print("total font data in flash: %d B (+ %d B of tf_font_t records)"
              % (total_bytes, 16 * len(parsed)))

    for w in warn:
        print("warning: %s" % w, file=sys.stderr)

    if args.compile_check:
        print("compile check:")
        compile_check(parsed, args.out, prefix, shared_header)

    if not args.quiet:
        print("wrote %d file(s):" % len(written))
        for p in written:
            print("  %s" % os.path.abspath(p))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (GoScanError, ValueError, AssertionError, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        sys.exit(1)
