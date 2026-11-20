#!/usr/bin/env python3
"""Compose PS4 package icons while preserving the original NULL_store bag PNG."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / "assets" / "brand"
SOURCE = BRAND / "nullstore-bag.png"
FONT_CANDIDATES = (
    Path("/System/Library/Fonts/Supplemental/Verdana Bold.ttf"),  # macOS
    Path("/usr/share/fonts/truetype/msttcorefonts/Verdana_Bold.ttf"),  # Linux + ttf-mscorefonts
    Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"),  # Linux (Ubuntu runners)
    ROOT / "assets" / "font.ttf",  # bundled fallback
)
FONT = next((p for p in FONT_CANDIDATES if p.exists()), FONT_CANDIDATES[-1])


def icon(label: str, target: Path) -> None:
    size = 512
    # A quiet, store-like emerald gradient with a soft pool of light behind the bag.
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    top, bottom = (3, 30, 22), (8, 64, 39)
    for y in range(size):
        t = y / (size - 1)
        for x in range(size):
            glow = max(0.0, 1.0 - (((x - 256) / 300) ** 2 + ((y - 185) / 360) ** 2))
            amount = min(1.0, t * 0.78 + glow * 0.12)
            pixels[x, y] = (
                int(top[0] * (1 - amount) + bottom[0] * amount),
                int(top[1] * (1 - amount) + bottom[1] * amount),
                int(top[2] * (1 - amount) + bottom[2] * amount),
                255,
            )

    bag = Image.open(SOURCE).convert("RGBA")
    # Keep the source artwork untouched apart from uniform scaling and placement.
    bag = bag.resize((338, 338), Image.Resampling.LANCZOS)
    image.alpha_composite(bag, (87, 22))

    scale = 3
    text_layer = Image.new("RGBA", (size * scale, size * scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(text_layer)
    font_size = 56 if label == "Store" else 52
    font = ImageFont.truetype(str(FONT), font_size * scale)
    box = draw.textbbox((0, 0), label, font=font)
    x = (size * scale - (box[2] - box[0])) // 2
    y = 408 * scale - box[1]
    draw.text((x + 1 * scale, y + 2 * scale), label, font=font, fill=(0, 12, 8, 150))
    draw.text((x, y), label, font=font, fill=(246, 255, 250, 255))
    text_layer = text_layer.resize((size, size), Image.Resampling.LANCZOS)
    image.alpha_composite(text_layer)
    target.parent.mkdir(parents=True, exist_ok=True)
    image.save(target, format="PNG", optimize=True)


def main() -> None:
    import os
    targets = (BRAND / "store-icon.png", BRAND / "service-icon.png", ROOT / "sce_sys" / "icon0.png")
    if os.environ.get("NULLSTORE_KEEP_ICONS") == "1" and all(t.exists() for t in targets):
        print("NULLSTORE_KEEP_ICONS=1: keeping committed icons.")
        return
    icon("Store", BRAND / "store-icon.png")
    icon("Service", BRAND / "service-icon.png")
    icon("Store", ROOT / "sce_sys" / "icon0.png")


if __name__ == "__main__":
    main()
