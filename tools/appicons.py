#!/usr/bin/env python3
"""
appicons.py: writes Zurbax's app icons, data/icons/apps/*.svg.

    tools/appicons.py [OUT_DIR]

Every icon is the same glass tile: a rounded square with continuous
corners, coloured from top to bottom, a gloss over its upper half, light on
its rim and a soft shadow under it. On the tile sits the app's glyph,
frosted white for the most part, lifted off the tile by a shadow of its
own. One file says what each app's tile and glyph are, so the set stays one
family: change the tile here and every icon follows.

Only what libzg's SVG renderer draws is used: paths, rects, circles,
ellipses, linear and radial gradients, opacity, transforms, dashes.
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "data", "icons", "apps")

W = "url(#gw)"  # the frosted white of glyphs


def squircle(cx, cy, half, n=4.6, steps=144):
    """A superellipse: a rounded square whose corners curve continuously."""
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        x = cx + half * math.copysign(abs(c) ** (2 / n), c)
        y = cy + half * math.copysign(abs(s) ** (2 / n), s)
        pts.append(f"{x:.2f} {y:.2f}")
    return "M" + " L".join(pts) + " Z"


def gear(cx, cy, teeth, outer, inner, hole):
    """A gear's outline, with its hole (fill-rule evenodd)."""
    pts = []
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        step = 2 * math.pi / teeth
        for frac, r in ((-0.30, inner), (-0.17, outer), (0.17, outer), (0.30, inner)):
            pts.append((cx + r * math.cos(a + frac * step), cy + r * math.sin(a + frac * step)))
    d = "M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + " Z"
    d += f" M{cx + hole} {cy} A{hole} {hole} 0 1 0 {cx - hole} {cy} A{hole} {hole} 0 1 0 {cx + hole} {cy} Z"
    return d


def sparkle(cx, cy, r):
    k = r * 0.16
    return (f"M{cx} {cy - r} C{cx + k} {cy - k} {cx + k} {cy - k} {cx + r} {cy} "
            f"C{cx + k} {cy + k} {cx + k} {cy + k} {cx} {cy + r} "
            f"C{cx - k} {cy + k} {cx - k} {cy + k} {cx - r} {cy} "
            f"C{cx - k} {cy - k} {cx - k} {cy - k} {cx} {cy - r} Z")


def folder_glyph(x, y, w, h, back, front):
    """A folder: its back with the tab, then the front, lower and wider."""
    tab = w * 0.38
    return [
        f'<path d="M{x} {y + 8}a7 7 0 0 1 7-7h{tab - 7}l{6} 6h{w - tab - 6 - 7}a7 7 0 0 1 7 7v{h - 8}H{x}z" fill="{back}"/>',
        f'<rect x="{x}" y="{y + h * 0.28}" width="{w}" height="{h * 0.72}" rx="7" fill="{front}"/>',
    ]


ICONS = {
    "arcade": ("#d58cff", "#7a1fc2", [
        '<rect x="30" y="76" width="68" height="22" rx="11" fill="{W}"/>',
        '<rect x="60" y="42" width="8" height="40" rx="4" fill="{W}" fill-opacity="0.85"/>',
        '<circle cx="64" cy="40" r="13" fill="url(#ball)"/>',
        '<circle cx="59.5" cy="35" r="4" fill="#ffffff" fill-opacity="0.7"/>',
        '<circle cx="42" cy="87" r="4" fill="#7a1fc2" fill-opacity="0.75"/>',
        '<circle cx="86" cy="87" r="4" fill="#ff4d6d"/>',
    ], ['<radialGradient id="ball" cx="0.4" cy="0.35" r="0.7"><stop offset="0" stop-color="#ff8fa3"/><stop offset="1" stop-color="#e11d48"/></radialGradient>']),

    "bridge": ("#62a8ff", "#5b21b6", [
        '<rect x="36" y="28" width="25" height="20" rx="4" fill="#bfe6ff"/>',
        '<rect x="67" y="28" width="25" height="20" rx="4" fill="#bfe6ff"/>',
        '<path d="M22 90h84" stroke="{W}" stroke-width="7" stroke-linecap="round"/>',
        '<path d="M28 90v-12c0-14 16-23 36-23s36 9 36 23v12" stroke="{W}" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>',
        '<path d="M46 90V71M64 90V67M82 90V71" stroke="{W}" stroke-width="5" stroke-linecap="round"/>',
        '<path d="M28 101h72" stroke="{W}" stroke-opacity="0.45" stroke-width="4" stroke-linecap="round"/>',
    ]),

    "calculator": ("#5b5b66", "#17171c", [
        '<rect x="32" y="26" width="64" height="16" rx="6" fill="{W}" fill-opacity="0.92"/>',
        '<rect x="66" y="31" width="24" height="6" rx="3" fill="#3f3f46"/>',
    ] + [
        f'<circle cx="{40 + c * 16}" cy="{54 + r * 14}" r="6" fill="{"#ff9f0a" if c == 3 else "{W}"}"'
        f'{"" if c == 3 or r == 0 else " fill-opacity=" + chr(34) + "0.4" + chr(34)}/>'
        for r in range(4) for c in range(4)
    ]),

    "calendar": ("#ffffff", "#e3e6ec", [
        '<rect x="30" y="24" width="68" height="14" rx="7" fill="#ef4444"/>',
        '<path d="M46 58l10-7v42" stroke="#1f2430" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>',
        '<path d="M68 51h22l-14 42" stroke="#1f2430" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>',
    ], [], 0.10),

    "camera": ("#eef2f7", "#9aa6b6", [
        '<rect x="47" y="34" width="28" height="16" rx="6" fill="#262c36"/>',
        '<rect x="26" y="42" width="76" height="52" rx="13" fill="#262c36"/>',
        '<circle cx="64" cy="68" r="19" fill="#4b5563"/>',
        '<circle cx="64" cy="68" r="13" fill="url(#lens)"/>',
        '<circle cx="59" cy="63" r="4" fill="#ffffff" fill-opacity="0.75"/>',
        '<circle cx="90" cy="53" r="3.5" fill="#fbbf24"/>',
    ], ['<radialGradient id="lens" cx="0.4" cy="0.35" r="0.75"><stop offset="0" stop-color="#7cc4ff"/><stop offset="1" stop-color="#1e3a8a"/></radialGradient>'], 0.08),

    "clock": ("#ffc187", "#e8590c", [
        '<circle cx="64" cy="64" r="36" fill="{W}"/>',
    ] + [
        f'<circle cx="{64 + 29 * math.cos(i * math.pi / 6):.2f}" cy="{64 + 29 * math.sin(i * math.pi / 6):.2f}" r="{2.6 if i % 3 == 0 else 1.6}" fill="#7c2d12" fill-opacity="0.55"/>'
        for i in range(12)
    ] + [
        '<path d="M64 64V45" stroke="#1f2430" stroke-width="5.5" stroke-linecap="round"/>',
        '<path d="M64 64l17 9" stroke="#1f2430" stroke-width="4" stroke-linecap="round"/>',
        '<path d="M64 64L51 84" stroke="#ea580c" stroke-width="2.2" stroke-linecap="round"/>',
        '<circle cx="64" cy="64" r="4" fill="#1f2430"/>',
    ]),

    "dashboard": ("#5fe3cf", "#0b7a70", [
        '<rect x="28" y="28" width="34" height="34" rx="9" fill="{W}"/>',
        '<circle cx="45" cy="45" r="9" stroke="#0d9488" stroke-width="4" stroke-opacity="0.35"/>',
        '<path d="M45 36a9 9 0 0 1 9 9" stroke="#0d9488" stroke-width="4" stroke-linecap="round"/>',
        '<rect x="66" y="28" width="34" height="34" rx="9" fill="{W}" fill-opacity="0.55"/>',
        '<rect x="28" y="66" width="72" height="34" rx="9" fill="{W}" fill-opacity="0.85"/>',
        '<path d="M40 90v-8M50 90V78M60 90v-6M70 90V76M80 90v-9M90 90V80" stroke="#0d9488" stroke-width="4" stroke-linecap="round"/>',
    ]),

    "drift": ("#7fd6ff", "#0361a8", [
        '<circle cx="64" cy="64" r="31" fill="{W}" fill-opacity="0.22"/>',
        '<circle cx="64" cy="64" r="31" stroke="{W}" stroke-width="5"/>',
        '<ellipse cx="64" cy="64" rx="13" ry="31" stroke="{W}" stroke-width="4"/>',
        '<path d="M34 64h60" stroke="{W}" stroke-width="4"/>',
        '<path d="M22 86C42 96 86 86 106 42" stroke="#ffd84d" stroke-width="6" stroke-linecap="round"/>',
    ]),

    "forge": ("#45454f", "#08080b", [
        '<path d="M36 44l18 16-18 16" stroke="#4ade80" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>',
        '<path d="M62 80h30" stroke="{W}" stroke-width="8" stroke-linecap="round"/>',
    ]),

    "harbor": ("#5ee08f", "#11803c", [
        '<circle cx="64" cy="33" r="7.5" stroke="{W}" stroke-width="5"/>',
        '<path d="M64 41v52" stroke="{W}" stroke-width="7" stroke-linecap="round"/>',
        '<path d="M50 53h28" stroke="{W}" stroke-width="6" stroke-linecap="round"/>',
        '<path d="M34 74c2 14 15 21 30 21s28-7 30-21" stroke="{W}" stroke-width="7" stroke-linecap="round"/>',
        '<path d="M26 79l8-13 9 10z" fill="{W}"/>',
        '<path d="M102 79l-8-13-9 10z" fill="{W}"/>',
    ]),

    "help": ("#6ee7f9", "#0a7da0", [
        '<circle cx="64" cy="64" r="27" stroke="{W}" stroke-width="15"/>',
        '<circle cx="64" cy="64" r="27" stroke="#f04438" stroke-width="15" stroke-dasharray="21.2 21.2"/>',
        '<circle cx="64" cy="64" r="34.5" stroke="#0e7490" stroke-opacity="0.25" stroke-width="1"/>',
        '<circle cx="64" cy="64" r="19.5" stroke="#0e7490" stroke-opacity="0.25" stroke-width="1"/>',
    ]),

    "installer": ("#ff9a5a", "#a8104f", [
        '<path d="M34 70v13a7 7 0 0 0 7 7h46a7 7 0 0 0 7-7V70" stroke="{W}" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>',
        '<path d="M64 28v42" stroke="{W}" stroke-width="8" stroke-linecap="round"/>',
        '<path d="M49 56l15 15 15-15" stroke="{W}" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>',
    ]),

    "mail": ("#4fc3ff", "#1257d6", [
        '<rect x="26" y="38" width="76" height="54" rx="10" fill="{W}"/>',
        '<path d="M31 45l33 25 33-25" stroke="#1d6ff2" stroke-opacity="0.55" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>',
    ]),

    "music": ("#ff7a93", "#c2123c", [
        '<path d="M53 37l40-9v12l-40 9z" fill="{W}"/>',
        '<rect x="50" y="40" width="7" height="47" rx="3" fill="{W}"/>',
        '<rect x="86" y="31" width="7" height="47" rx="3" fill="{W}"/>',
        '<ellipse cx="45" cy="87" rx="11" ry="8.5" fill="{W}" transform="rotate(-20 45 87)"/>',
        '<ellipse cx="81" cy="78" rx="11" ry="8.5" fill="{W}" transform="rotate(-20 81 78)"/>',
    ]),

    "notes": ("#ffe36e", "#f2a81d", [
        '<path d="M38 40h30" stroke="#8a4b08" stroke-opacity="0.7" stroke-width="7" stroke-linecap="round"/>',
        '<path d="M38 58h52M38 73h52M38 88h34" stroke="{W}" stroke-width="6" stroke-linecap="round"/>',
    ]),

    "photos": ("#ffa58a", "#7b2fd0", [
        '<rect x="27" y="33" width="74" height="62" rx="11" fill="{W}" fill-opacity="0.22"/>',
        '<rect x="27" y="33" width="74" height="62" rx="11" stroke="{W}" stroke-width="5"/>',
        '<path d="M33 89l21-26 12 14 10-10 19 22z" fill="{W}"/>',
        '<circle cx="82" cy="50" r="7.5" fill="#ffe14d"/>',
    ]),

    "pulse": ("#3a4a63", "#0c1426", [
        '<path d="M30 46h68M30 64h68M30 82h68" stroke="{W}" stroke-opacity="0.12" stroke-width="2"/>',
        '<path d="M26 68h16l8-22 11 42 10-32 7 12h24" stroke="#34e39a" stroke-width="6.5" stroke-linecap="round" stroke-linejoin="round"/>',
    ]),

    "screenshot": ("#8f97ff", "#3a2fc0", [
        '<path d="M34 52V41a7 7 0 0 1 7-7h11M76 34h11a7 7 0 0 1 7 7v11M94 76v11a7 7 0 0 1-7 7H76M52 94H41a7 7 0 0 1-7-7V76" stroke="{W}" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>',
        '<circle cx="64" cy="64" r="10" fill="{W}"/>',
    ]),

    "screentime": ("#c58bff", "#5b1a9c", [
        '<rect x="38" y="26" width="52" height="8" rx="4" fill="{W}"/>',
        '<rect x="38" y="94" width="52" height="8" rx="4" fill="{W}"/>',
        '<path d="M46 34c0 18 14 23 14 30s-14 12-14 30h36c0-18-14-23-14-30s14-12 14-30z" fill="{W}" fill-opacity="0.28"/>',
        '<path d="M46 34c0 18 14 23 14 30s-14 12-14 30h36c0-18-14-23-14-30s14-12 14-30z" stroke="{W}" stroke-width="4" stroke-linejoin="round"/>',
        '<path d="M51 41h26c-1 8-9 13-13 17-4-4-12-9-13-17z" fill="#ffd66b"/>',
        '<path d="M51 90c3-8 9-11 13-13 4 2 10 5 13 13z" fill="#ffd66b"/>',
        '<path d="M64 58v19" stroke="#ffd66b" stroke-width="2"/>',
    ]),

    "settings": ("#a3abb8", "#495261", [
        f'<path d="{gear(64, 64, 9, 36, 27, 12)}" fill="{{W}}" fill-rule="evenodd"/>',
        '<circle cx="64" cy="64" r="20" stroke="#495261" stroke-opacity="0.25" stroke-width="2"/>',
    ]),

    "steam": ("#2f5f86", "#121a29", [
        '<path d="M42 46h44a15 15 0 0 1 14.6 11.6l5 21.5a9.5 9.5 0 0 1-16.5 8.2L82 80H46l-7.1 7.3a9.5 9.5 0 0 1-16.5-8.2l5-21.5A15 15 0 0 1 42 46z" fill="{W}"/>',
        '<path d="M44 57v14M37 64h14" stroke="#1d3350" stroke-width="5" stroke-linecap="round"/>',
        '<circle cx="82" cy="59" r="4.5" fill="#3b82f6"/>',
        '<circle cx="91" cy="68" r="4.5" fill="#ef4444"/>',
    ]),

    "vault": ("#5fb2ff", "#1d5fd8",
              folder_glyph(26, 32, 76, 62, "{W}\" fill-opacity=\"0.55", "{W}") + [
                  '<path d="M52 72h24" stroke="#1d6ff2" stroke-opacity="0.45" stroke-width="5" stroke-linecap="round"/>',
              ]),

    "weather": ("#7fd0ff", "#2f6fe8", [
        '<circle cx="78" cy="48" r="18" fill="url(#sun)"/>',
        '<path d="M42 92a14 14 0 0 1 2-28a20 20 0 0 1 38-2a15.5 15.5 0 0 1 4 30z" fill="{W}"/>',
    ], ['<linearGradient id="sun" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff07a"/><stop offset="1" stop-color="#ffab1a"/></linearGradient>']),

    "welcome": ("#ffd05a", "#e0306e", [
        f'<path d="{sparkle(60, 66, 34)}" fill="{{W}}"/>',
        f'<path d="{sparkle(92, 34, 11)}" fill="{{W}}" fill-opacity="0.9"/>',
        f'<path d="{sparkle(34, 96, 7)}" fill="{{W}}" fill-opacity="0.8"/>',
    ]),

    "writer": ("#6fc3ff", "#4338ca", [
        '<rect x="30" y="26" width="50" height="66" rx="8" fill="{W}"/>',
        '<path d="M40 42h28M40 54h30M40 66h22" stroke="#4f46e5" stroke-opacity="0.45" stroke-width="4.5" stroke-linecap="round"/>',
        '<g transform="rotate(38 82 76)">'
        '<rect x="76" y="44" width="13" height="40" rx="2" fill="#ffd54d"/>'
        '<rect x="76" y="37" width="13" height="8" rx="3" fill="#ff7aa8"/>'
        '<path d="M76 84l6.5 15 6.5-15z" fill="#f6d2a8"/>'
        '<path d="M80.4 94l2.1 5 2.1-5z" fill="#1f2430"/>'
        '</g>',
    ]),
}


def tile_svg(name, top, bottom, glyph, extra_defs=(), edge=0.0):
    glyph = [g.replace("{W}", W) for g in glyph]
    # the glyph's shadow: the same shapes, black, a little lower
    def blacken(s):
        s = re.sub(r'(fill|stroke)="(?!none)[^"]*"', r'\1="#000000"', s)
        return s
    shadow = "".join(blacken(g) for g in glyph)
    tile = squircle(64, 63, 54)
    rim = squircle(64, 63, 53.25)
    defs = [
        '<radialGradient id="drop" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#000000" stop-opacity="0.34"/><stop offset="1" stop-color="#000000" stop-opacity="0"/></radialGradient>',
        f'<linearGradient id="bg" x1="0" y1="0" x2="0.35" y2="1"><stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient>',
        '<linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity="0.34"/><stop offset="0.44" stop-color="#ffffff" stop-opacity="0.07"/><stop offset="0.52" stop-color="#ffffff" stop-opacity="0"/><stop offset="1" stop-color="#000000" stop-opacity="0.14"/></linearGradient>',
        '<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#ffffff" stop-opacity="0.22"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/></radialGradient>',
        '<linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity="0.85"/><stop offset="0.22" stop-color="#ffffff" stop-opacity="0.22"/><stop offset="0.7" stop-color="#ffffff" stop-opacity="0.04"/><stop offset="1" stop-color="#ffffff" stop-opacity="0.32"/></linearGradient>',
        '<linearGradient id="gw" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#e6ecf5"/></linearGradient>',
        *extra_defs,
    ]
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" fill="none">',
        f"<!-- {name}: written by tools/appicons.py -->",
        "<defs>" + "".join(defs) + "</defs>",
        '<ellipse cx="64" cy="118" rx="46" ry="7" fill="url(#drop)"/>',
        f'<path d="{tile}" fill="url(#bg)"/>',
        f'<path d="{tile}" fill="url(#gloss)"/>',
        '<ellipse cx="64" cy="30" rx="42" ry="15" fill="url(#glow)"/>',
        f'<g transform="translate(0 2.5)" opacity="0.2">{shadow}</g>',
        *glyph,
        f'<path d="{rim}" stroke="url(#rim)" stroke-width="1.5"/>',
    ]
    if edge:
        out.append(f'<path d="{tile}" stroke="#000000" stroke-opacity="{edge}" stroke-width="1"/>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


def folder_svg():
    """The folder icon, for folders rather than an app: no tile, the glass is the folder."""
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" fill="none">',
        "<!-- folder: written by tools/appicons.py -->",
        "<defs>"
        '<radialGradient id="drop" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#000000" stop-opacity="0.32"/><stop offset="1" stop-color="#000000" stop-opacity="0"/></radialGradient>'
        '<linearGradient id="back" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3f86e8"/><stop offset="1" stop-color="#1d4fa8"/></linearGradient>'
        '<linearGradient id="front" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#8ccbff"/><stop offset="0.5" stop-color="#5ea9f7"/><stop offset="1" stop-color="#3b82e6"/></linearGradient>'
        '<linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity="0.9"/><stop offset="0.3" stop-color="#ffffff" stop-opacity="0.15"/><stop offset="1" stop-color="#ffffff" stop-opacity="0.3"/></linearGradient>'
        '<linearGradient id="paper" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#dfe6f0"/></linearGradient>'
        "</defs>",
        '<ellipse cx="64" cy="112" rx="52" ry="7" fill="url(#drop)"/>',
        '<path d="M12 32a8 8 0 0 1 8-8h26l9 9h53a8 8 0 0 1 8 8v60a8 8 0 0 1-8 8H20a8 8 0 0 1-8-8z" fill="url(#back)"/>',
        '<rect x="24" y="36" width="80" height="40" rx="3" fill="url(#paper)" transform="rotate(-3 64 56)"/>',
        '<rect x="10" y="48" width="108" height="61" rx="9" fill="url(#front)"/>',
        '<rect x="10.75" y="48.75" width="106.5" height="59.5" rx="8.25" stroke="url(#rim)" stroke-width="1.5"/>',
        '<path d="M50 72h28" stroke="#1d4fa8" stroke-opacity="0.35" stroke-width="5" stroke-linecap="round"/>',
        "</svg>",
    ]) + "\n"


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, spec in ICONS.items():
        top, bottom, glyph = spec[0], spec[1], spec[2]
        extra = spec[3] if len(spec) > 3 else []
        edge = spec[4] if len(spec) > 4 else 0.0
        with open(os.path.join(OUT, name + ".svg"), "w") as f:
            f.write(tile_svg(name, top, bottom, glyph, extra, edge))
    with open(os.path.join(OUT, "folder.svg"), "w") as f:
        f.write(folder_svg())


if __name__ == "__main__":
    main()
