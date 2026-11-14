# Generates res/app.ico: a glossy glass prism tile.
from PIL import Image, ImageDraw, ImageFilter
import sys
S = 256
def make(size):
    sc = 4; N = size * sc
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    # gradient tile
    grad = Image.new("RGBA", (N, N))
    gp = grad.load()
    for y in range(N):
        for x in range(N):
            t = (x + y) / (2 * N)
            r = int(64 + (191 - 64) * t); g = int(132 + (90 - 132) * t); b = int(255 + (242 - 255) * t)
            gp[x, y] = (r, g, b, 255)
    mask = Image.new("L", (N, N), 0)
    pad = int(N * 0.06); rad = int(N * 0.24)
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, N - pad, N - pad], rad, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # glass highlight
    hl = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(hl).ellipse([-N * 0.3, -N * 0.75, N * 1.1, N * 0.5], fill=(255, 255, 255, 70))
    hm = Image.new("L", (N, N), 0); hm.paste(mask)
    img = Image.alpha_composite(img, Image.composite(hl, Image.new("RGBA", (N, N)), hm))
    d = ImageDraw.Draw(img)
    # prism triangle
    cx, cy = N / 2, N * 0.53
    tri = [(cx, cy - N * 0.25), (cx + N * 0.24, cy + N * 0.17), (cx - N * 0.24, cy + N * 0.17)]
    d.polygon(tri, fill=(255, 255, 255, 235))
    inner = [(cx, cy - N * 0.13), (cx + N * 0.13, cy + N * 0.1), (cx - N * 0.13, cy + N * 0.1)]
    d.polygon(inner, fill=(120, 110, 250, 255))
    d.rounded_rectangle([pad, pad, N - pad, N - pad], rad, outline=(255, 255, 255, 120), width=max(1, int(N * 0.012)))
    return img.resize((size, size), Image.LANCZOS)
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
big = make(256)
big.save(sys.argv[1], sizes=[(s, s) for s in sizes], append_images=[make(s) for s in sizes[:-1]])
big.save(sys.argv[1].replace(".ico", ".png"))
