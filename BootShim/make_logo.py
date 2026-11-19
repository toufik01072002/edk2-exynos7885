#!/usr/bin/env python3
import sys
import struct

WIDTH = 256
HEIGHT = 256

def generate_default_logo():
    """Génère un logo UEFI / Exynos 7885 propre (256x256 BGRA) sans dépendance externe."""
    buf = bytearray(WIDTH * HEIGHT * 4)
    cx, cy = WIDTH // 2, HEIGHT // 2

    for y in range(HEIGHT):
        for x in range(WIDTH):
            dx = x - cx
            dy = y - cy
            dist2 = dx * dx + dy * dy
            idx = (y * WIDTH + x) * 4

            # Fond noir pur (B=0, G=0, R=0, A=255)
            b, g, r, a = 0, 0, 0, 255

            # Anneau extérieur bleu électrique (#1E90FF -> B=255, G=144, R=30)
            if 105 * 105 <= dist2 <= 116 * 116:
                b, g, r = 255, 144, 30
            # Cercle intérieur sombre (#121824)
            elif dist2 < 105 * 105:
                b, g, r = 36, 24, 18

            # Emblème géométrique "E7" / puce centrale blanche & cyan
            if -45 <= dx <= 45 and -45 <= dy <= 45:
                # Bordure du carré central
                if abs(dx) >= 39 or abs(dy) >= 39:
                    b, g, r = 255, 220, 80
                # Barres horizontales style "E" / UEFI
                elif (-25 <= dx <= 25) and (
                    (-26 <= dy <= -16) or (-5 <= dy <= 5) or (16 <= dy <= 26)
                ):
                    if dx <= -10 or (-26 <= dy <= -16) or (-5 <= dy <= 5) or (16 <= dy <= 26):
                        b, g, r = 255, 255, 255

            buf[idx:idx+4] = struct.pack("<BBBB", b, g, r, a)

    return bytes(buf)

def convert_image(img_path):
    from PIL import Image
    img = Image.open(img_path).convert("RGBA").resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    raw = bytearray(WIDTH * HEIGHT * 4)
    pixels = img.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b, a = pixels[x, y]
            # Fond noir si transparence
            r = (r * a) // 255
            g = (g * a) // 255
            b = (b * a) // 255
            idx = (y * WIDTH + x) * 4
            raw[idx:idx+4] = struct.pack("<BBBB", b, g, r, 255)
    return bytes(raw)

if __name__ == "__main__":
    out_path = "BootShim/logo.bin"
    if len(sys.argv) > 1:
        data = convert_image(sys.argv[1])
        print(f"[OK] Image '{sys.argv[1]}' convertie en {out_path} ({len(data)} octets)")
    else:
        data = generate_default_logo()
        print(f"[OK] Logo par défaut généré dans {out_path} ({len(data)} octets)")

    with open(out_path, "wb") as f:
        f.write(data)
