"""Generate the UniNet performance graphs for the README.

Data-driven: reads `uninet_bench_log.csv` (written/appended by `tests/benchmark.cpp`)
and plots the latest run's 4096-vert rows. If the log is absent, it errors with the
command to produce it. This replaces UniVox's hand-pasted numbers with a real
collect-then-plot pipeline:

    ./build/benchmark 200 2000     # writes uninet_bench_log.csv (+ uninet_profile.txt)
    python3 tools/plot_benchmark.py
"""
import csv
import os
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = Path(__file__).resolve().parent.parent / "docs"
LOG = Path(__file__).resolve().parent.parent / "uninet_bench_log.csv"


def load_latest_4096():
    if not LOG.exists():
        sys.exit(f"No benchmark log at {LOG}. Run: ./build/benchmark 200 2000")
    rows = {}
    for r in csv.DictReader(LOG.open()):
        if int(r["verts"]) != 4096:
            continue
        rows[r["op"]] = r  # later rows overwrite -> latest run wins
    if not rows:
        sys.exit(f"No 4096-vert rows in {LOG}.")
    return rows


def f(r, key):
    return float(r[key])


R = load_latest_4096()
payload_kib = f(R["encode"], "payload_kib") + 0.0  # all 4096 rows share this

# ── chart 1: codec + transport throughput ──
ops = [
    ("encode", "encode"),
    ("decode", "decode"),
    ("compress_lz4", "LZ4\ncompress"),
    ("decompress_lz4", "LZ4\ndecompress"),
    ("decompress_zlib", "zlib\ndecompress"),
    ("loopback_pubsub", "loopback\npub/sub"),
]
mbps = [f(R[k], "mbps") for k, _ in ops]
colors = ["#1565c0", "#1565c0", "#2f7d32", "#2f7d32", "#9aa7b4", "#ef6c00"]

fig, ax = plt.subplots(figsize=(9.0, 4.8), dpi=140)
bars = ax.bar(range(len(ops)), mbps, color=colors, edgecolor="black", linewidth=0.5)
ax.set_yscale("log")
ax.set_ylabel("Throughput (MB/s, uncompressed) — log scale")
ax.set_title(f"UniNet codec + transport throughput (4096-vert mesh, ~{payload_kib:.0f} KiB)")
ax.set_xticks(range(len(ops)))
ax.set_xticklabels([lbl for _, lbl in ops], fontsize=9)
ax.grid(axis="y", alpha=0.3, which="both")
for r, v in zip(bars, mbps):
    ax.annotate(f"{v/1000:.1f} GB/s" if v >= 1000 else f"{v:.0f} MB/s",
                (r.get_x() + r.get_width() / 2, v), ha="center", va="bottom", fontsize=8.5)
fig.tight_layout()
fig.savefig(OUT / "benchmark_throughput.png")
print("wrote", OUT / "benchmark_throughput.png")

# ── chart 2: compression tiers (wire size + ratio) ──
none_w = payload_kib
zlib_w = none_w / f(R["compress_zlib"], "extra")
lz4_w = none_w / f(R["compress_lz4"], "extra")
tiers = ["None", "zlib", "LZ4"]
wire_kib = [none_w, zlib_w, lz4_w]
ratio = [1.00, f(R["compress_zlib"], "extra"), f(R["compress_lz4"], "extra")]
ccol = ["#9aa7b4", "#9aa7b4", "#2f7d32"]

fig, ax = plt.subplots(figsize=(7.6, 4.4), dpi=140)
x = range(len(tiers))
bars = ax.bar(x, wire_kib, color=ccol, edgecolor="black", linewidth=0.5, width=0.6)
ax.axhline(payload_kib, color="gray", linewidth=0.8, linestyle="--",
           label=f"uncompressed ({payload_kib:.0f} KiB)")
ax.set_ylabel("Wire size (KiB) — lower is better")
ax.set_title("Compression tiers: wire size vs ratio")
ax.set_xticks(list(x))
ax.set_xticklabels(tiers)
ax.set_ylim(0, payload_kib * 1.18)
ax.legend(frameon=False, loc="upper right")
ax.grid(axis="y", alpha=0.3)
for r, w, rt in zip(bars, wire_kib, ratio):
    ax.annotate(f"{w:.0f} KiB\n({rt:.2f}x)", (r.get_x() + r.get_width() / 2, w),
                ha="center", va="bottom", fontsize=9)
fig.tight_layout()
fig.savefig(OUT / "benchmark_compression.png")
print("wrote", OUT / "benchmark_compression.png")
