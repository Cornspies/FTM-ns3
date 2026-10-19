#!/usr/bin/env python3
"""Compare FTM results of the ns-3.33 original (OLD_DIR) with those of the ns-3.47 port (NEW_DIR).

Usage: python3 compare_ftm.py OLD_DIR NEW_DIR

Files are paired by their path relative to OLD_DIR and NEW_DIR. They are the text files written
by ftm-ranging (one line per FTM dialog: "rtt_ps sig_str_dBm") and ftm-localization (same, plus
the "x y" position, after a "#" header line). An RTT of 0 marks a dialog with missing time stamps.

The FTM error models seed their random generator from std::random_device, so RTT values differ
from run to run in both versions. The comparison therefore checks
  - the structure: number of dialogs and which of them are valid (RTT != 0), line by line,
  - the signal strengths (and positions) line by line; these are deterministic unless the channel
    has random fading (sig_str_fading),
  - the distribution of the valid RTTs: mean difference in standard errors and a two-sample
    Kolmogorov-Smirnov test.
Byte-identical files are reported as IDENTICAL (expected for runs with fixed seeds).
"""

import math
import pathlib
import sys


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            rows.append([float(v) for v in line.split()])
    return rows


def mean_sd(values):
    n = len(values)
    if n == 0:
        return float("nan"), float("nan")
    mean = sum(values) / n
    var = sum((v - mean) ** 2 for v in values) / (n - 1) if n > 1 else 0.0
    return mean, math.sqrt(var)


def ks_pvalue(lam):
    """Kolmogorov distribution Q_KS(lambda) (Numerical Recipes probks)."""
    a2 = -2.0 * lam * lam
    fac, total, previous = 2.0, 0.0, 0.0
    for j in range(1, 101):
        term = fac * math.exp(a2 * j * j)
        total += term
        if abs(term) <= 0.001 * previous or abs(term) <= 1e-8 * total:
            return min(max(total, 0.0), 1.0)
        fac = -fac
        previous = abs(term)
    return 1.0


def ks_test(a, b):
    """Two-sample Kolmogorov-Smirnov test; returns (D, approximate p-value)."""
    a, b = sorted(a), sorted(b)
    n1, n2 = len(a), len(b)
    if n1 == 0 or n2 == 0:
        return float("nan"), float("nan")
    i = j = 0
    d = 0.0
    while i < n1 and j < n2:
        x = min(a[i], b[j])
        while i < n1 and a[i] <= x:
            i += 1
        while j < n2 and b[j] <= x:
            j += 1
        d = max(d, abs(i / n1 - j / n2))
    ne = n1 * n2 / (n1 + n2)
    return d, ks_pvalue((math.sqrt(ne) + 0.12 + 0.11 / math.sqrt(ne)) * d)


def compare(old_path, new_path):
    if old_path.read_bytes() == new_path.read_bytes():
        rows = load(old_path)
        valid = [r[0] for r in rows if r[0] != 0]
        mean, _ = mean_sd(valid)
        return dict(lines=(len(rows), len(rows)), zeros=(len(rows) - len(valid),) * 2,
                    pattern=0, sig=0, pos=0, means=(mean, mean), z=0.0, ks_p=1.0,
                    verdict="IDENTICAL")

    old, new = load(old_path), load(new_path)
    n = min(len(old), len(new))
    pattern = sum((old[i][0] == 0) != (new[i][0] == 0) for i in range(n))
    sig = sum(abs(old[i][1] - new[i][1]) > 1e-6 for i in range(n) if len(old[i]) > 1)
    pos = sum(old[i][2:] != new[i][2:] for i in range(n))
    old_valid = [r[0] for r in old if r[0] != 0]
    new_valid = [r[0] for r in new if r[0] != 0]
    (m_old, sd_old), (m_new, sd_new) = mean_sd(old_valid), mean_sd(new_valid)
    se = math.sqrt(sd_old**2 / max(len(old_valid), 1) + sd_new**2 / max(len(new_valid), 1))
    z = (m_new - m_old) / se if se > 0 else 0.0
    _, p = ks_test(old_valid, new_valid)

    same_structure = len(old) == len(new) and pattern == 0 and pos == 0
    same_distribution = abs(z) < 4 and p > 0.001
    if same_structure and same_distribution:
        verdict = "EQUIVALENT"
    elif same_distribution:
        verdict = "EQUIVALENT (stat.)"
    else:
        verdict = "CHECK"
    return dict(lines=(len(old), len(new)), zeros=(len(old) - len(old_valid), len(new) - len(new_valid)),
                pattern=pattern, sig=sig, pos=pos, means=(m_old, m_new), z=z, ks_p=p, verdict=verdict)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    old_dir, new_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    old_files = {p.relative_to(old_dir) for p in old_dir.rglob("*") if p.is_file()}
    new_files = {p.relative_to(new_dir) for p in new_dir.rglob("*") if p.is_file()}
    common = sorted(old_files & new_files)
    if not common:
        sys.exit("no files present in both directories")

    header = (f"{'file':32} {'dialogs':>13} {'zero RTTs':>13} {'pattern':>7} {'sig':>6} "
              f"{'mean RTT old/new [ps]':>23} {'diff':>7} {'KS p':>6}  verdict")
    print(header)
    print("-" * len(header))
    verdicts = {}
    for rel in common:
        r = compare(old_dir / rel, new_dir / rel)
        verdicts[r["verdict"]] = verdicts.get(r["verdict"], 0) + 1
        print(f"{str(rel):32} {r['lines'][0]:>6}/{r['lines'][1]:<6} {r['zeros'][0]:>6}/{r['zeros'][1]:<6} "
              f"{r['pattern']:>7} {r['sig']:>6} {r['means'][0]:>11.0f}/{r['means'][1]:<11.0f} "
              f"{r['z']:>+6.1f}σ {r['ks_p']:>6.3f}  {r['verdict']}")
    print()
    print("pattern: dialogs valid in one version but not in the other")
    print("sig:     dialogs whose signal strength differs; expected for sig_str_fading (the fading is")
    print("         drawn from ns-3's own RNG, whose streams differ between versions) and for positions")
    print("         exactly at the 1 m reference distance of ThreeLogDistance (ns-3.47 computes")
    print("         distances with std::hypot instead of sqrt, so they can round to the other side)")
    print("diff:    difference of the mean valid RTT, in standard errors")
    print("summary:", ", ".join(f"{v} {k}" for k, v in sorted(verdicts.items())))
    only_old, only_new = len(old_files - new_files), len(new_files - old_files)
    if only_old or only_new:
        print(f"not compared: {only_old} file(s) only in {old_dir}, {only_new} only in {new_dir}")


if __name__ == "__main__":
    main()
