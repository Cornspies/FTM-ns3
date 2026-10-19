#!/usr/bin/env python3
"""Plot the comparison of FTM results between the ns-3.33 original and the ns-3.47 port.

Usage: python3 plot_compare.py RESULTS_DIR

RESULTS_DIR holds ranging/, localization/ and seeded/, each with old/ and new/
subdirectories as written by the comparison runs (see compare_ftm.py); missing
parts are skipped. Writes PNGs to RESULTS_DIR/plots. Zero RTTs (failed dialogs, see the zero-RTT
bug) are dropped everywhere; only valid measurements are plotted.
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import stats
from scipy.optimize import minimize

if len(sys.argv) != 2:
    sys.exit(__doc__)
RES = os.path.expanduser(sys.argv[1])
OUT = os.path.join(RES, "plots")

MODELS = ["wired", "wireless", "sig_str", "sig_str_fading"]
DISTANCES = [1, 5, 10, 20, 50]
VERSIONS = {"old": ("ns-3.33 (original)", "#2a78d6"), "new": ("ns-3.47 (port)", "#eb6834")}

INK, INK_2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": INK_2, "axes.titlecolor": INK,
    "axes.titlesize": 11, "axes.titleweight": "bold", "axes.labelsize": 9,
    "xtick.color": INK_2, "ytick.color": INK_2, "xtick.labelsize": 8, "ytick.labelsize": 8,
    "axes.grid": True, "axes.axisbelow": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False, "legend.fontsize": 9, "font.size": 9,
})


def rtt_to_m(rtt_ps):
    """Round-trip time in ps -> one-way distance in m."""
    return np.asarray(rtt_ps, dtype=float) / 2 * 3e-4


def load(path):
    """Columns of a result file (rtt, sig_str[, x, y]); None if missing or empty."""
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        return None
    return np.atleast_2d(np.loadtxt(path, comments="#"))


def ranging_errors(base, ver, model, d):
    data = load(f"{base}/{ver}/{model}/{d:03d}m")
    if data is None:
        return np.array([])
    rtt = data[:, 0]
    return rtt_to_m(rtt[rtt != 0]) - d


def figure_legend(fig, extra=()):
    handles = [plt.Line2D([], [], color=c, lw=6, solid_capstyle="butt") for _, c in VERSIONS.values()]
    labels = [label for label, _ in VERSIONS.values()]
    for h, label in extra:
        handles.append(h)
        labels.append(label)
    fig.legend(handles, labels, loc="upper right", ncol=len(labels), bbox_to_anchor=(0.98, 0.955))


def save(fig, name):
    path = os.path.join(OUT, name)
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print("wrote", path)


def plot_ranging_error(base, title, name):
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.5))
    for ax, model in zip(axes.flat, MODELS):
        for i, (ver, (_, color)) in enumerate(VERSIONS.items()):
            data = [ranging_errors(base, ver, model, d) for d in DISTANCES]
            pos = np.arange(len(DISTANCES)) + (i - 0.5) * 0.36
            bp = ax.boxplot(data, positions=pos, widths=0.3, patch_artist=True, showfliers=True,
                            medianprops=dict(color=SURFACE, lw=1.5),
                            boxprops=dict(facecolor=color, edgecolor=color),
                            whiskerprops=dict(color=color, lw=1), capprops=dict(color=color, lw=1),
                            flierprops=dict(marker="o", ms=2, mfc=color, mec="none", alpha=0.4))
        ax.axhline(0, color=INK_2, lw=1, ls="--")
        ax.set_xticks(np.arange(len(DISTANCES)), [f"{d} m" for d in DISTANCES])
        ax.set_title(model)
        ax.grid(axis="x", visible=False)
    for ax in axes[:, 0]:
        ax.set_ylabel("ranging error: measured − true [m]")
    for ax in axes[1]:
        ax.set_xlabel("true distance")
    fig.suptitle(title, x=0.02, ha="left", fontsize=13, fontweight="bold", color=INK)
    figure_legend(fig, [(plt.Line2D([], [], color=INK_2, lw=1, ls="--"), "no error")])
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    save(fig, name)


def plot_ranging_ecdf(base):
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.5), sharey=True)
    for ax, model in zip(axes.flat, MODELS):
        errs = {}
        for ver, (_, color) in VERSIONS.items():
            e = np.sort(np.concatenate([ranging_errors(base, ver, model, d) for d in DISTANCES]))
            errs[ver] = e
            if len(e):
                ax.step(e, np.arange(1, len(e) + 1) / len(e), where="post", color=color, lw=2)
        if all(len(e) for e in errs.values()):
            p = stats.ks_2samp(errs["old"], errs["new"]).pvalue
            ax.text(0.98, 0.05, f"KS p = {p:.2f}\nn = {len(errs['old'])} / {len(errs['new'])}",
                    transform=ax.transAxes, ha="right", va="bottom", color=INK_2, fontsize=8)
        ax.set_title(model)
    for ax in axes[:, 0]:
        ax.set_ylabel("fraction of dialogs")
    for ax in axes[1]:
        ax.set_xlabel("ranging error [m], all distances pooled")
    fig.suptitle("Ranging error distribution: old vs new", x=0.02, ha="left",
                 fontsize=13, fontweight="bold", color=INK)
    figure_legend(fig)
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    save(fig, "ranging_ecdf.png")


def multilaterate(dists, anchors):
    """Least-squares position from distances to anchors (same method as ftm_localization/visualize.py)."""
    def err(x):
        return np.sum((np.linalg.norm(anchors - x, axis=1) - dists) ** 2)
    return minimize(err, anchors.mean(axis=0), method="Nelder-Mead").x


def localization_errors(path):
    """Error of the estimated AP position (true: origin) using the first 3..10 STA positions."""
    data = load(path)
    if data is None:
        return {}
    anchors, dists = [], []
    # rows come in blocks per STA position; keep the position order of the file
    _, first = np.unique(data[:, 2:4], axis=0, return_index=True)
    for i in sorted(first):
        x, y = data[i, 2:4]
        rows = data[(data[:, 2] == x) & (data[:, 3] == y)]
        rtt = rows[rows[:, 0] != 0, 0]
        if len(rtt):
            anchors.append((x, y))
            dists.append(rtt_to_m(rtt.mean()))
    anchors, dists = np.array(anchors), np.array(dists)
    return {k: np.linalg.norm(multilaterate(dists[:k], anchors[:k]))
            for k in range(3, len(dists) + 1)}


def plot_localization():
    base = os.path.join(RES, "localization")
    errs = sorted(d for d in os.listdir(os.path.join(base, "old")) if d.startswith("err"))
    names = {"err0": "wired", "err1": "wireless", "err2": "sig_str", "err3": "sig_str_fading"}
    fig, axes = plt.subplots(1, len(errs), figsize=(5.5 * len(errs), 4.5), sharey=True, squeeze=False)
    for ax, e in zip(axes[0], errs):
        seeds = sorted(os.listdir(os.path.join(base, "old", e)))
        for ver, (_, color) in VERSIONS.items():
            per_seed = [localization_errors(os.path.join(base, ver, e, s)) for s in seeds]
            ks = sorted(set().union(*per_seed))
            for res in per_seed:   # individual seeds, faint
                ax.plot(list(res), list(res.values()), color=color, lw=1, alpha=0.3)
            mean = [np.mean([r[k] for r in per_seed if k in r]) for k in ks]
            ax.plot(ks, mean, color=color, lw=2, marker="o", ms=8, mec=SURFACE, mew=2)
        ax.set_title(f"{names.get(e, e)} ({len(seeds)} seeds)")
        ax.set_xlabel("STA positions used for multilateration")
    axes[0, 0].set_ylabel("AP position error [m]")
    axes[0, 0].set_ylim(bottom=0)
    fig.suptitle("Localization error: old vs new", x=0.02, ha="left",
                 fontsize=13, fontweight="bold", color=INK)
    figure_legend(fig, [(plt.Line2D([], [], color=INK_2, lw=1, alpha=0.4), "single seed")])
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    save(fig, "localization.png")


def plot_seeded_scatter():
    base = os.path.join(RES, "seeded")
    fig, axes = plt.subplots(2, 2, figsize=(11, 9))
    for ax, model in zip(axes.flat, MODELS):
        old_all, new_all = [], []
        for d in DISTANCES:
            old, new = load(f"{base}/old/{model}/{d:03d}m"), load(f"{base}/new/{model}/{d:03d}m")
            if old is None or new is None or len(old) != len(new):
                continue
            both = (old[:, 0] != 0) & (new[:, 0] != 0)
            old_all.append(rtt_to_m(old[both, 0]))
            new_all.append(rtt_to_m(new[both, 0]))
        if not old_all:
            continue
        o, n = np.concatenate(old_all), np.concatenate(new_all)
        lim = (min(o.min(), n.min()) - 1, max(o.max(), n.max()) + 1)
        ax.plot(lim, lim, color=INK_2, lw=1, ls="--")
        ax.scatter(o, n, s=10, color=VERSIONS["new"][1], edgecolors="none", alpha=0.5)
        same = np.mean(o == n)
        ax.text(0.03, 0.97, f"{same:.0%} of {len(o)} RTTs identical", transform=ax.transAxes,
                ha="left", va="top", color=INK_2, fontsize=9)
        ax.set_xlim(lim)
        ax.set_ylim(lim)
        ax.set_aspect("equal")
        ax.set_title(model)
        ax.set_xlabel("ns-3.33 measured distance [m]")
        ax.set_ylabel("ns-3.47 measured distance [m]")
    fig.suptitle("Fixed seeds: same dialog, old vs new (dashed = identical)", x=0.02, ha="left",
                 fontsize=13, fontweight="bold", color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    save(fig, "seeded_scatter.png")


def main():
    os.makedirs(OUT, exist_ok=True)
    if os.path.isdir(os.path.join(RES, "ranging")):
        plot_ranging_error(os.path.join(RES, "ranging"), "Ranging error per distance: old vs new",
                           "ranging_error.png")
        plot_ranging_ecdf(os.path.join(RES, "ranging"))
    if os.path.isdir(os.path.join(RES, "localization")):
        plot_localization()
    if os.path.isdir(os.path.join(RES, "seeded")):
        plot_seeded_scatter()


if __name__ == "__main__":
    main()
