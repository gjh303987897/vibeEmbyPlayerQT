#!/usr/bin/env python3
"""Generate the app's toolbar glyph set (icon/glyphs/*.svg).

House style - deliberately close to Apple's SF Symbols rather than to lucide/feather:

  * 24-unit grid, but shapes only occupy a ~14 unit optical box (lucide uses 18-20), so the row
    looks airy and the icons read as one weight instead of a mix of crowded and sparse marks.
  * Single hairline: 1.7 units (0.071 of the grid; SF "regular" is ~1.5/22 = 0.068) with round
    caps and joins everywhere, including corners of open strokes.
  * Corner radius ~0.26 of a shape's side (Apple's rounded squares are generous, not 2-unit).
  * Dots are filled circles with r ~ 0.8 x stroke, not zero-length "h.01" hacks (Qt's SVG
    renderer drops those, which is how lucide's ellipsis-style dots usually disappear).
  * Geometry is computed from parameters below - no hand-written path data - because arcs,
    arrowheads and the 45-degree pencil need exact trigonometry to stay on the hairline grid.

The rasterized output is verified with the offscreen QtQml grab described in
VIBEDOCS/MediaServices.md (ink box per glyph + measured stroke thickness).

Usage:  python scripts/gen_icons.py [output_dir]     # default: icon/glyphs
"""

import math
import os
import sys

GRID = 24.0
STROKE = 1.7
# Optical box: closed shapes span this many units of the 24 grid.
BOX = 14.4
HALF = BOX / 2.0
DOT_R = STROKE * 0.8
RADIUS_RATIO = 0.26


def f(v):
    """Format a coordinate with the least precision that still survives Qt's parser."""
    s = ('%.3f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('', '-0') else s


def line(x1, y1, x2, y2):
    return 'M%s %sL%s %s' % (f(x1), f(y1), f(x2), f(y2))


def polyline(points, close=False):
    pts = list(points)
    d = 'M' + ' '.join(f(v) for v in pts[0])
    for p in pts[1:]:
        d += 'L' + ' '.join(f(v) for v in p)
    if close:
        d += 'Z'
    return d


def rect(x, y, w, h, r):
    """Rounded rectangle as an explicit path (arc corners), which keeps the hairline continuous."""
    r = min(r, w / 2.0, h / 2.0)
    return (
        'M%s %s' % (f(x + r), f(y))
        + 'H%s' % f(x + w - r)
        + 'A%s %s 0 0 1 %s %s' % (f(r), f(r), f(x + w), f(y + r))
        + 'V%s' % f(y + h - r)
        + 'A%s %s 0 0 1 %s %s' % (f(r), f(r), f(x + w - r), f(y + h))
        + 'H%s' % f(x + r)
        + 'A%s %s 0 0 1 %s %s' % (f(r), f(r), f(x), f(y + h - r))
        + 'V%s' % f(y + r)
        + 'A%s %s 0 0 1 %s %s' % (f(r), f(r), f(x + r), f(y))
        + 'Z'
    )


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def arc(cx, cy, r, a0, a1):
    """Circular arc from a0 to a1 degrees; angles grow clockwise on screen (y is down)."""
    p0 = polar(cx, cy, r, a0)
    p1 = polar(cx, cy, r, a1)
    delta = (a1 - a0) % 360.0
    large = 1 if delta > 180.0 else 0
    return 'M%s %sA%s %s 0 %s 1 %s %s' % (
        f(p0[0]), f(p0[1]), f(r), f(r), large, f(p1[0]), f(p1[1]))


def arrow_at(cx, cy, r, deg, size=2.6, spread=42.0):
    """Two arrowhead legs touching the arc end point, pointing along the clockwise tangent."""
    tip = polar(cx, cy, r, deg)
    tangent = deg + 90.0
    legs = []
    for sign in (-1, 1):
        back = polar(0, 0, size, tangent + 180.0 + sign * spread)
        legs.append('M%s %sL%s %s' % (f(tip[0]), f(tip[1]),
                                      f(tip[0] + back[0]), f(tip[1] + back[1])))
    return ''.join(legs)


def centered(shape_half):
    return 12.0 - shape_half, 12.0 + shape_half


# ---------------------------------------------------------------------------------------------
# Glyph geometry. Each entry returns a list of (kind, data) where kind is 'd' (stroked path) or
# 'dot' (filled circle: (cx, cy, r)).
# ---------------------------------------------------------------------------------------------

def plus():
    a, b = centered(6.8)
    return [('d', line(a, 12, b, 12)), ('d', line(12, a, 12, b))]


def minus():
    a, b = centered(6.4)
    return [('d', line(a, 12, b, 12))]


def x():
    a, b = centered(5.6)
    return [('d', line(a, a, b, b)), ('d', line(b, a, a, b))]


def check():
    return [('d', 'M%s %sL%s %sL%s %s' % (f(5.2), f(12.6), f(9.7), f(17.1), f(18.8), f(6.4)))]


def square():
    x0 = 12 - HALF
    return [('d', rect(x0, x0, BOX, BOX, BOX * RADIUS_RATIO))]


def copy():
    """Restore glyph: front rounded square plus the visible top-left corner of the one behind it.

    The back corner is pushed clear of the front square on purpose - at 18px nominal the strokes are
    only ~1.3px apart, so a 2-unit gap merges the two marks into one blob.
    """
    front_x, front_y, side = 7.8, 8.4, 11.4
    back_r = 3.0
    return [
        ('d', rect(front_x, front_y, side, side, side * RADIUS_RATIO)),
        ('d', 'M%s %sV%sA%s %s 0 0 1 %s %sH%s' % (
            f(4.2), f(15.0), f(6.6), f(back_r), f(back_r),
            f(6.6), f(4.2), f(14.2))),
    ]


def lock():
    body_w, body_h = 13.2, 9.6
    body_x, body_y = 12 - body_w / 2.0, 10.4
    shackle_r = 3.7
    left_x = 12 - shackle_r
    right_x = 12 + shackle_r
    return [
        ('d', rect(body_x, body_y, body_w, body_h, body_w * 0.24)),
        ('d', 'M%s %sV%sA%s %s 0 0 1 %s %sV%s' % (
            f(left_x), f(body_y), f(7.7), f(shackle_r), f(shackle_r),
            f(right_x), f(7.7), f(body_y))),
    ]


def lock_open():
    """Same body, shackle lifted and released on the right (SF's lock.open)."""
    body_w, body_h = 13.2, 9.6
    body_x, body_y = 12 - body_w / 2.0, 10.4
    shackle_r = 3.7
    left_x = 12 - shackle_r
    return [
        ('d', rect(body_x, body_y, body_w, body_h, body_w * 0.24)),
        ('d', 'M%s %sV%sA%s %s 0 0 1 %s %s' % (
            f(left_x), f(body_y), f(7.5), f(shackle_r), f(shackle_r),
            f(12 + shackle_r + 1.9), f(4.6))),
    ]


def ellipsis():
    return [('dot', (5.8, 12, DOT_R)), ('dot', (12, 12, DOT_R)), ('dot', (18.2, 12, DOT_R))]


def sliders():
    """Settings/preferences: three tracks, each crossed by a short handle (SF slider.horizontal.3)."""
    tracks = [(6.4, 9.4), (12.0, 15.6), (17.6, 7.6)]
    out = []
    for y, handle_x in tracks:
        out.append(('d', line(4.4, y, 19.6, y)))
        out.append(('d', line(handle_x, y - 2.3, handle_x, y + 2.3)))
    return out


def pencil():
    """45-degree pencil built from the tip point, axis direction and half width."""
    tip = (5.0, 19.0)
    ux, uy = math.cos(math.radians(-45)), math.sin(math.radians(-45))
    nx, ny = -uy, ux
    length, half_w, collar = 15.4, 2.35, 3.6

    def pt(along, across):
        return (tip[0] + ux * along + nx * across, tip[1] + uy * along + ny * across)

    c1, c2 = pt(collar, half_w), pt(collar, -half_w)
    d1, d2 = pt(length, half_w), pt(length, -half_w)
    return [
        ('d', polyline([c1, d1, d2, c2, tip], close=True)),
        ('d', line(c1[0], c1[1], c2[0], c2[1])),
    ]


def refresh():
    """Two counter-symmetric arcs, each ending in a small head. A single 310-degree arc with one head
    was tried first and read worse: at toolbar size the head merges with the arc's round cap into a
    solid wedge, while two short arcs keep the circle legible."""
    r = 7.1
    return [
        ('d', arc(12, 12, r, -150, -26)),
        ('d', arrow_at(12, 12, r, -26)),
        ('d', arc(12, 12, r, 30, 154)),
        ('d', arrow_at(12, 12, r, 154)),
    ]


def clock():
    return [
        ('d', 'M%s %sA%s %s 0 1 1 %s %sA%s %s 0 1 1 %s %sZ' % (
            f(12 + 7.4), f(12), f(7.4), f(7.4), f(12 - 7.4), f(12),
            f(7.4), f(7.4), f(12 + 7.4), f(12))),
        ('d', 'M%s %sV%sL%s %s' % (f(12), f(7.1), f(12), f(15.7), f(14.2))),
    ]


def chart():
    bars = [(6.2, 13.4), (12, 7.2), (17.8, 10.6)]
    return [('d', line(x, 18.6, x, top)) for x, top in bars]


def shield_dots():
    """Rounded-top shield with a soft point, plus three filled dots (privacy card editor)."""
    top_y, shoulder_y, bottom_y = 3.4, 6.0, 20.6
    half_w = 7.0
    d = (
        'M%s %s' % (f(12 - half_w), f(shoulder_y))
        + 'V%sA%s %s 0 0 1 %s %s' % (f(top_y + 1.6), f(2.6), f(2.6), f(12 - half_w + 2.4), f(top_y))
        + 'H%sA%s %s 0 0 1 %s %s' % (f(12 + half_w - 2.4), f(2.6), f(2.6),
                                     f(12 + half_w), f(top_y + 1.6))
        + 'V%s' % f(shoulder_y)
        + 'C%s %s %s %s %s %s' % (f(12 + half_w), f(15.6), f(16.4), f(18.4), f(12), f(bottom_y))
        + 'C%s %s %s %s %s %s' % (f(12 - half_w), f(18.4), f(12 - half_w), f(15.6),
                                  f(12 - half_w), f(shoulder_y))
        + 'Z'
    )
    return [('d', d),
            ('dot', (8.7, 11.2, DOT_R * 0.92)),
            ('dot', (12, 11.2, DOT_R * 0.92)),
            ('dot', (15.3, 11.2, DOT_R * 0.92))]


GLYPHS = {
    'plus': plus,
    'minus': minus,
    'x': x,
    'check': check,
    'square': square,
    'copy': copy,
    'lock': lock,
    'lock-open': lock_open,
    'ellipsis': ellipsis,
    'sliders': sliders,
    'pencil': pencil,
    'refresh': refresh,
    'clock': clock,
    'chart': chart,
    'shield-dots': shield_dots,
}

HEADER = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"'
    ' fill="none" stroke="#ffffff" stroke-width="%s" stroke-linecap="round"'
    ' stroke-linejoin="round">' % f(STROKE)
)


def render(parts):
    body = []
    for kind, data in parts:
        if kind == 'd':
            body.append('  <path d="%s"/>' % data)
        else:
            cx, cy, r = data
            body.append('  <circle cx="%s" cy="%s" r="%s" fill="#ffffff" stroke="none"/>'
                        % (f(cx), f(cy), f(r)))
    return HEADER + '\n' + '\n'.join(body) + '\n</svg>\n'


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'icon', 'glyphs')
    out = out.replace('\\', '/')
    os.makedirs(out, exist_ok=True)
    names = []
    for name, fn in sorted(GLYPHS.items()):
        parts = fn()
        path = os.path.join(out, name + '.svg')
        with open(path, 'w', newline='\n') as handle:
            handle.write(render(parts))
        names.append(name)
    print('grid=%s stroke=%s optical box=%s -> %d glyphs in %s' % (
        f(GRID), f(STROKE), f(BOX), len(names), out))
    print(' '.join(names))


if __name__ == '__main__':
    main()
