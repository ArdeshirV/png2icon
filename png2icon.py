#!/usr/bin/env python3
"""Convert a PNG or JPEG image into a multi-resolution Windows icon."""
import argparse
import ctypes
import ctypes.util
import struct
import sys
from pathlib import Path


class PngImage(ctypes.Structure):
    # Public prefix of libpng's simplified image structure; the rest is opaque.
    _fields_ = [
        ("opaque", ctypes.c_void_p), ("version", ctypes.c_uint),
        ("width", ctypes.c_uint), ("height", ctypes.c_uint),
        ("format", ctypes.c_uint), ("flags", ctypes.c_uint),
        ("colormap_entries", ctypes.c_uint), ("warning_or_error", ctypes.c_uint),
        ("message", ctypes.c_char * 64), ("opaque_storage", ctypes.c_ubyte * 256),
    ]


def png_rgba(path: Path):
    library = ctypes.util.find_library("png16") or ctypes.util.find_library("png")
    if not library:
        raise RuntimeError("libpng is required to read PNG images")
    png = ctypes.CDLL(library)
    begin = png.png_image_begin_read_from_file
    begin.argtypes = [ctypes.POINTER(PngImage), ctypes.c_char_p]
    begin.restype = ctypes.c_int
    finish = png.png_image_finish_read
    finish.argtypes = [ctypes.POINTER(PngImage), ctypes.c_void_p, ctypes.c_void_p,
                       ctypes.c_int, ctypes.c_void_p]
    finish.restype = ctypes.c_int
    free = png.png_image_free
    free.argtypes = [ctypes.POINTER(PngImage)]

    image = PngImage()
    image.version = 1
    if not begin(ctypes.byref(image), str(path).encode()):
        raise RuntimeError("libpng could not read the input file")
    image.format = 0x03  # PNG_FORMAT_RGBA
    pixels = (ctypes.c_ubyte * (image.width * image.height * 4))()
    if not finish(ctypes.byref(image), None, pixels, 0, None):
        message = bytes(image.message).split(b"\0", 1)[0].decode(errors="replace")
        free(ctypes.byref(image))
        raise RuntimeError(f"libpng could not decode the input: {message}")
    width, height = image.width, image.height
    free(ctypes.byref(image))
    return width, height, bytes(pixels)


def image_rgba(path: Path):
    if path.suffix.lower() == ".png":
        return png_rgba(path)
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "JPEG input requires Pillow; install it with 'python3 -m pip install Pillow'"
        ) from exc
    with Image.open(path) as image:
        rgba = image.convert("RGBA")
        return rgba.width, rgba.height, rgba.tobytes()


def resize_rgba(src, sw, sh, dw, dh):
    # Bilinear resampling with premultiplied-alpha interpolation to avoid
    # dark fringes around transparent icon edges.
    output = bytearray(dw * dh * 4)
    for y in range(dh):
        fy = (y + .5) * sh / dh - .5
        y0 = max(0, min(sh - 1, int(fy)))
        y1 = min(sh - 1, y0 + 1)
        wy = max(0.0, min(1.0, fy - int(fy)))
        for x in range(dw):
            fx = (x + .5) * sw / dw - .5
            x0 = max(0, min(sw - 1, int(fx)))
            x1 = min(sw - 1, x0 + 1)
            wx = max(0.0, min(1.0, fx - int(fx)))
            samples = []
            for xx, yy, weight in ((x0,y0,(1-wx)*(1-wy)), (x1,y0,wx*(1-wy)),
                                   (x0,y1,(1-wx)*wy), (x1,y1,wx*wy)):
                i = (yy * sw + xx) * 4
                a = src[i+3] / 255.0
                samples.append((src[i] * a, src[i+1] * a, src[i+2] * a, src[i+3], weight))
            alpha = sum(s[3] * s[4] for s in samples)
            oi = (y * dw + x) * 4
            if alpha > 0:
                for c in range(3):
                    premul = sum(s[c] * s[4] for s in samples)
                    output[oi+c] = max(0, min(255, round(premul * 255 / alpha)))
            output[oi+3] = max(0, min(255, round(alpha)))
    return bytes(output)


def dib_image(rgba, size):
    # ICO DIB uses bottom-up BGRA pixels plus a 1-bit AND mask.
    xor = bytearray()
    mask = bytearray()
    stride = (size * 4 + 3) & ~3
    mask_stride = ((size + 31) // 32) * 4
    for y in range(size - 1, -1, -1):
        row = bytearray()
        mask_row = bytearray(mask_stride)
        for x in range(size):
            i = (y * size + x) * 4
            r, g, b, a = rgba[i:i+4]
            row.extend((b, g, r, a))
            if a < 128:
                mask_row[x // 8] |= 0x80 >> (x % 8)
        xor.extend(row)
        xor.extend(b'\0' * (stride - len(row)))
        mask.extend(mask_row)
    header = struct.pack('<IIIHHIIIIII', 40, size, size * 2, 1, 32, 0,
                         len(xor) + len(mask), 0, 0, 0, 0)
    return header + xor + mask


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="source PNG or JPEG image")
    parser.add_argument("output", nargs="?", type=Path,
                        help="output ICO file (default: input filename with .ico extension)")
    args = parser.parse_args(argv)
    source = args.input
    destination = args.output or source.with_suffix(".ico")
    try:
        if source.suffix.lower() not in {".png", ".jpg", ".jpeg"}:
            raise ValueError("input must have a .png, .jpg, or .jpeg extension")
        sw, sh, pixels = image_rgba(source)
        sizes = (256, 128, 64, 48, 32, 16)
        images = [(size, dib_image(resize_rgba(pixels, sw, sh, size, size), size))
                  for size in sizes]
        offset = 6 + 16 * len(images)
        directory = bytearray(struct.pack('<HHH', 0, 1, len(images)))
        payload = bytearray()
        for size, data in images:
            directory.extend(struct.pack('<BBBBHHII', size & 255, size & 255, 0, 0,
                                         1, 32, len(data), offset))
            payload.extend(data)
            offset += len(data)
        destination.write_bytes(directory + payload)
        print(f'Created {destination} ({len(images)} sizes)')
    except Exception as exc:
        print(f'Could not create {destination}: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
