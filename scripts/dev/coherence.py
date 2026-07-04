#!/usr/bin/env python3
# Coherence-dynamics logo, v2 (refinement of the concentric "coh1" direction).
# Reading outward = increasing information / effective dimension:
#   luminous smooth CORE   = concentration-of-measure limit (high-D collapses to low-D order)
#   FRACTAL BAND           = the interesting in-between (power-law / self-similar roughness)
#   irregular NEEDLE SPIKES = high-D, high-information state (uneven spectrum is the point)
# Deterministic (index-based variation, no RNG) so the mark is reproducible.

import math

CX = CY = 512.0


def poly_path(points, closed=True):
    d = "M {:.2f} {:.2f} ".format(*points[0])
    for x, y in points[1:]:
        d += "L {:.2f} {:.2f} ".format(x, y)
    if closed:
        d += "Z"
    return d


def polar_points(rfunc, n=720):
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        r = rfunc(t)
        pts.append((CX + r * math.cos(t), CY - r * math.sin(t)))
    return pts


def weier(t, base, amp, H=0.62, b=2, M=6, ph=0.0):
    s = norm = 0.0
    for i in range(M):
        a = H ** i
        s += a * math.cos((b ** i) * t + ph + i * 1.3)
        norm += a
    return base * (1 + amp * s / norm)


def needle(a, r_tip, r_base=235.0, dw=0.052):
    """Thin triangle: apex at the tip, base straddling the fractal-shell edge."""
    tip = (CX + r_tip * math.cos(a), CY - r_tip * math.sin(a))
    bl = (CX + r_base * math.cos(a - dw), CY - r_base * math.sin(a - dw))
    br = (CX + r_base * math.cos(a + dw), CY - r_base * math.sin(a + dw))
    return [bl, tip, br]


SPIKES = 16
needle_paths = []
for i in range(SPIKES):
    a = 2 * math.pi * i / SPIKES - math.pi / 2
    # uneven "information spectrum": deterministic varied length, a few dominant modes
    v = 0.5 * abs(math.sin(i * 2.399 + 0.6)) + 0.5 * abs(math.sin(i * 0.77 + 1.9))
    r_tip = 360 + 120 * v
    needle_paths.append(poly_path(needle(a, r_tip)))

band_outer = polar_points(lambda t: weier(t, 258, 0.15, H=0.6, b=2, M=6))
contour1 = polar_points(lambda t: weier(t, 224, 0.12, H=0.62, b=2, M=6, ph=0.9))
contour2 = polar_points(lambda t: weier(t, 192, 0.09, H=0.64, b=2, M=6, ph=1.8))
core = polar_points(lambda t: 150 * (1 + 0.025 * math.cos(3 * t + 0.3)))

needles_svg = "\n    ".join(
    f'<path d="{p}" fill="url(#spike)" opacity="0.92"/>' for p in needle_paths
)

svg = f'''<?xml version="1.0" encoding="UTF-8"?>
<svg width="1024" height="1024" viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#151B30"/><stop offset="1" stop-color="#090D1A"/>
    </linearGradient>
    <radialGradient id="glow" cx="0.5" cy="0.42" r="0.7">
      <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.10"/>
      <stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="spike" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#7FE0E4"/><stop offset="1" stop-color="#2C6A79"/>
    </radialGradient>
    <radialGradient id="band" cx="0.5" cy="0.45" r="0.6">
      <stop offset="0" stop-color="#3E96A0"/><stop offset="1" stop-color="#276C7C"/>
    </radialGradient>
    <radialGradient id="coreGlow" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#F2FBFF"/><stop offset="0.5" stop-color="#8CEDE6"/>
      <stop offset="1" stop-color="#3EBFC4"/>
    </radialGradient>
  </defs>
  <rect x="100" y="100" width="824" height="824" rx="185" fill="url(#bg)"/>
  <rect x="100" y="100" width="824" height="824" rx="185" fill="url(#glow)"/>
  <g>
    {needles_svg}
    <path d="{poly_path(band_outer)}" fill="url(#band)"/>
    <path d="{poly_path(contour1)}" fill="none" stroke="#59B7BE" stroke-width="3" opacity="0.45"/>
    <path d="{poly_path(contour2)}" fill="none" stroke="#7CD2D4" stroke-width="2.5" opacity="0.35"/>
    <circle cx="512" cy="512" r="168" fill="#0C1322" opacity="0.35"/>
    <path d="{poly_path(core)}" fill="url(#coreGlow)"/>
  </g>
</svg>'''

with open("coh1v2.svg", "w") as f:
    f.write(svg)
print("wrote coh1v2")
