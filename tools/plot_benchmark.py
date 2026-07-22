"""Generate the UniNet performance graphs for the README.

Data collected by `tests/benchmark.cpp` on a 32-core box (4096-vert mesh, ~108 KiB
uncompressed CBOR, mean of 300 reps). Reproduce: `cmake --build build --target
benchmark && ./build/benchmark 4096 300`.
"""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "docs"

# ── measured numbers (build/benchmark 4096 300) ──
payload_kib = 108.3

# Codec / transport throughput (MB/s of uncompressed payload).
ops = ["encode", "decode", "LZ4\ncompress", "LZ4\ndecompress", "zlib\ndecompress", "loopback\npub/sub"]
mbps = [1343, 7786, 14495, 11098, 503, 899]   # encode/decode/lz4-c/lz4-d/zlib-d/loopback
colors = ["#1565c0", "#1565c0", "#2f7d32", "#2f7d32", "#9aa7b4", "#ef6c00"]

fig, ax = plt.subplots(figsize=(9.0, 4.8), dpi=140)
bars = ax.bar(range(len(ops)), mbps, color=colors, edgecolor="black", linewidth=0.5)
ax.set_yscale("log")
ax.set_ylabel("Throughput (MB/s, uncompressed) — log scale")
ax.set_title("UniNet codec + transport throughput (4096-vert mesh, ~108 KiB)")
ax.set_xticks(range(len(ops)))
ax.set_xticklabels(ops, fontsize=9)
ax.grid(axis="y", alpha=0.3, which="both")
for r, v in zip(bars, mbps):
    ax.annotate(f"{v/1000:.1f} GB/s" if v >= 1000 else f"{v} MB/s",
                (r.get_x() + r.get_width() / 2, v), ha="center", va="bottom", fontsize=8.5)
fig.tight_layout()
fig.savefig(OUT / "benchmark_throughput.png")
print("wrote", OUT / "benchmark_throughput.png")

# ── compression tiers: wire size + ratio ──
tiers = ["None", "zlib", "LZ4"]
wire_kib = [payload_kib, 47.4, 76.4]
ratio = [1.00, 2.29, 1.42]
ccol = ["#9aa7b4", "#9aa7b4", "#2f7d32"]

fig, ax = plt.subplots(figsize=(7.6, 4.4), dpi=140)
x = range(len(tiers))
bars = ax.bar(x, wire_kib, color=ccol, edgecolor="black", linewidth=0.5, width=0.6)
ax.axhline(payload_kib, color="gray", linewidth=0.8, linestyle="--", label="uncompressed (108.3 KiB)")
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
