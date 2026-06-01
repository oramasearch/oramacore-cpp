#!/usr/bin/env python3
"""Render the memory comparison (C++ in-memory vs Rust/OramaCore on-disk) as a
dependency-free SVG bar chart — no matplotlib (Homebrew Python 3.14 has no wheel).

Two side-by-side LINEAR panels (10k and 2M) so the small 10k bars stay readable
without a log scale. Numbers come from bench/RESULTS.md (measured 2026-06-01,
M3 Max). Re-run the benchmarks, update DATA below, then: python3 plot_memory.py

Output: bench/memory-results.svg
"""
import os

# (cpp, rust) MB per metric. C++ build spike is None — its build is in-place.
DATA = {
    "10k (10,000 docs)": {
        "peak process RSS":   (17, 73),
        "index footprint":    (4, 2),
        "build-time RAM":     (None, 63),
    },
    "2M (2,000,000 docs)": {
        "peak process RSS":   (2041, 9425),
        "index footprint":    (403, 388),
        "build-time RAM":     (None, 8719),
    },
}
CPP, RUST = "#3b82f6", "#f59e0b"
TXT, GRID, AXIS = "#1f2937", "#e5e7eb", "#9ca3af"

def fmt(mb):
    return f"{mb/1000:.1f} GB" if mb >= 1000 else f"{mb:.0f} MB"

def nice_top(v):
    # round the axis max up to a clean 1/2/5 * 10^k
    import math
    if v <= 0: return 1
    e = 10 ** math.floor(math.log10(v))
    for m in (1, 2, 2.5, 5, 10):
        if m * e >= v: return m * e
    return 10 * e

def panel(x0, y0, w, h, title, metrics):
    top = nice_top(max(v for pair in metrics.values() for v in pair if v))
    out = [f'<text x="{x0+w/2:.0f}" y="{y0-12}" text-anchor="middle" '
           f'font-weight="700" font-size="15" fill="{TXT}">{title}</text>']
    # y grid + ticks
    nticks = 4
    for i in range(nticks + 1):
        val = top * i / nticks
        yy = y0 + h - h * i / nticks
        out.append(f'<line x1="{x0}" y1="{yy:.1f}" x2="{x0+w}" y2="{yy:.1f}" '
                   f'stroke="{GRID}" stroke-width="1"/>')
        out.append(f'<text x="{x0-8}" y="{yy+4:.1f}" text-anchor="end" '
                   f'font-size="11" fill="{AXIS}">{fmt(val)}</text>')
    # grouped bars
    ngroup = len(metrics)
    gw = w / ngroup
    bw = gw * 0.30
    for gi, (label, (cpp, rust)) in enumerate(metrics.items()):
        gx = x0 + gi * gw + gw / 2
        for bi, (val, color, name) in enumerate(
                ((cpp, CPP, "C++"), (rust, RUST, "Rust"))):
            bx = gx - bw - 2 + bi * (bw + 4)  # C++ left, Rust right, 4px gap, centered
            if val is None:
                out.append(f'<text x="{bx+bw/2:.1f}" y="{y0+h-6}" '
                           f'text-anchor="middle" font-size="10" fill="{AXIS}" '
                           f'transform="rotate(-90 {bx+bw/2:.1f} {y0+h-6})">in-place</text>')
                continue
            bh = h * val / top
            by = y0 + h - bh
            out.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" '
                       f'height="{bh:.1f}" fill="{color}" rx="2"/>')
            out.append(f'<text x="{bx+bw/2:.1f}" y="{by-4:.1f}" '
                       f'text-anchor="middle" font-size="10" font-weight="600" '
                       f'fill="{TXT}">{fmt(val)}</text>')
        out.append(f'<text x="{gx:.1f}" y="{y0+h+16}" text-anchor="middle" '
                   f'font-size="11" fill="{TXT}">{label}</text>')
    return "\n".join(out)

def main():
    W, H = 920, 380
    pw, ph = 360, 250
    p1x, p2x, py = 70, 510, 70
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
             f'<rect width="{W}" height="{H}" fill="white"/>',
             f'<text x="{W/2}" y="28" text-anchor="middle" font-weight="800" '
             f'font-size="18" fill="{TXT}">Memory: oramacore-cpp (in-memory) vs '
             f'OramaCore/Rust (on-disk) — single-field BM25, M3 Max</text>']
    panels = list(DATA.items())
    parts.append(panel(p1x, py, pw, ph, panels[0][0], panels[0][1]))
    parts.append(panel(p2x, py, pw, ph, panels[1][0], panels[1][1]))
    # legend
    lx, ly = W/2 - 90, H - 22
    parts.append(f'<rect x="{lx}" y="{ly}" width="14" height="14" fill="{CPP}" rx="2"/>')
    parts.append(f'<text x="{lx+20}" y="{ly+12}" font-size="12" fill="{TXT}">C++ (in-memory)</text>')
    parts.append(f'<rect x="{lx+150}" y="{ly}" width="14" height="14" fill="{RUST}" rx="2"/>')
    parts.append(f'<text x="{lx+170}" y="{ly+12}" font-size="12" fill="{TXT}">Rust / OramaCore (on-disk)</text>')
    parts.append("</svg>")
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "memory-results.svg")
    with open(out, "w") as f:
        f.write("\n".join(parts))
    print("wrote", out)

if __name__ == "__main__":
    main()
