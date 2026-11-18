"""Driver for the bench17 harness: quality, time (locked, interleaved) and report subcommands."""
import argparse
import csv
import hashlib
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "bench"
RESULTS = BUILD / "results"
LOCK = ROOT / "build" / "bench.lock"
MANIFEST = ROOT / "tests" / "metrics" / "bench17_manifest.json"
INPUTS = Path(r"C:\Users\regis\Documents\Codex\2026-09-14\continue-the-work-on-c-users-2\work\baseline_protocol_20261114\scalar_reference\inputs")
EXE = BUILD / "bench17.exe"
CODECS = ("official", "current")


def load_manifest():
    return json.loads(MANIFEST.read_text())


def select(manifest, ids):
    cases = manifest["cases"]
    if not ids:
        return cases
    wanted = ids.split(",")
    missing = set(wanted) - {c["id"] for c in cases}
    if missing:
        sys.exit(f"unknown cases: {sorted(missing)}")
    return [c for c in cases if c["id"] in wanted]


def verify_inputs(cases, inputs):
    seen = {}
    for c in cases:
        if c["pcm"] in seen:
            continue
        digest = hashlib.sha256((inputs / c["pcm"]).read_bytes()).hexdigest()
        if digest != c["sha256"]:
            sys.exit(f"sha256 mismatch for {c['pcm']}: {digest} != {c['sha256']}")
        seen[c["pcm"]] = True


def invoke(mode, codec, c, inputs, exe, extra=()):
    cmd = [str(exe), mode, "--codec", codec, "--app", c["application"], "--channels", c["channels"], "--bitrate", c["bitrate"],
           "--complexity", c["complexity"], "--vbr", c["vbr"], "--fec", c["fec"], "--loss", c["loss"], "--frame", c["frame"],
           "--pcm", inputs / c["pcm"], *extra]
    r = subprocess.run([str(x) for x in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{c['id']} {codec} {mode} failed: {r.stderr.strip()}")
    return json.loads(r.stdout)


def stamp():
    return time.strftime("%Y%m%d_%H%M%S")


def write_outputs(kind, data, rows, md=None):
    RESULTS.mkdir(parents=True, exist_ok=True)
    base = RESULTS / f"{stamp()}_{kind}"
    base.with_suffix(".json").write_text(json.dumps(data, indent=1))
    if rows:
        fields = list(dict.fromkeys(k for r in rows for k in r))
        with base.with_suffix(".csv").open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            w.writerows(rows)
    if md:
        base.with_suffix(".md").write_text(md)
    print(base.with_suffix(".json"))
    return base


def cmd_quality(a):
    manifest = load_manifest()
    cases = select(manifest, a.cases)
    inputs = Path(a.inputs)
    verify_inputs(cases, inputs)
    metrics = list(manifest["metrics"])
    out, rows = {}, []
    for c in cases:
        off_runs = [invoke("quality", "official", c, inputs, a.exe) for _ in range(a.official_reps)]
        cur = invoke("quality", "current", c, inputs, a.exe)
        keys = ["actual_bitrate_bps", "packets", "bytes", *metrics]
        spread = {k: max(r[k] for r in off_runs) - min(r[k] for r in off_runs) for k in keys}
        out[c["id"]] = {"official": off_runs[0], "official_runs": off_runs, "official_spread": spread, "current": cur}
        print(f"{c['id']}: official spread max={max(spread.values()):.3g} " + " ".join(f"{k}={v:.3g}" for k, v in spread.items() if v))
        rows.append({"case": c["id"], "codec": "official", **off_runs[0]})
        rows.append({"case": c["id"], "codec": "current", **cur})
        rows.append({"case": c["id"], "codec": "official_spread", **spread})
    write_outputs("quality", {"upstream_pin": manifest["upstream_pin"], "official_reps": a.official_reps, "cases": out}, rows)


class Lock:
    def __enter__(self):
        LOCK.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(LOCK, "a+")
        try:
            if sys.platform == "win32":
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            sys.exit(f"another bench17 time run holds {LOCK}; refusing to run concurrently")
        return self

    def __exit__(self, *exc):
        if sys.platform == "win32":
            import msvcrt
            self.f.seek(0)
            msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
        self.f.close()


def default_batch(c):
    return 8 if c["channels"] == 2 else 30


def cmd_time(a):
    manifest = load_manifest()
    cases = select(manifest, a.cases)
    inputs = Path(a.inputs)
    verify_inputs(cases, inputs)
    samples = {c["id"]: {k: [] for k in CODECS} for c in cases}
    batches = {c["id"]: a.batch or default_batch(c) for c in cases}
    with Lock():
        for rep in range(a.reps):
            order = cases[rep % len(cases):] + cases[:rep % len(cases)]
            for c in order:
                codecs = CODECS if rep % 2 == 0 else CODECS[::-1]
                for codec in codecs:
                    r = invoke("time", codec, c, inputs, a.exe, ["--batch", batches[c["id"]], "--cpu", a.cpu])
                    samples[c["id"]][codec].append(r["cpu_ms_per_clip"])
            print(f"rep {rep + 1}/{a.reps} done", flush=True)
    out, rows = {}, []
    for c in cases:
        s = samples[c["id"]]
        med = {k: statistics.median(s[k]) for k in CODECS}
        margin = (med["official"] - med["current"]) / med["official"] * 100.0
        out[c["id"]] = {"batch": batches[c["id"]], "samples_ms_per_clip": s, "median_ms_per_clip": med, "speed_margin_pct": margin}
        rows.append({"case": c["id"], "batch": batches[c["id"]], "official_median_ms": med["official"], "current_median_ms": med["current"], "speed_margin_pct": margin})
        print(f"{c['id']}: official {med['official']:.2f} ms, current {med['current']:.2f} ms, margin {margin:+.2f}%")
    write_outputs("time", {"upstream_pin": manifest["upstream_pin"], "reps": a.reps, "cpu": a.cpu, "cases": out}, rows)


def latest(kind):
    files = sorted(RESULTS.glob(f"*_{kind}.json"))
    if not files:
        sys.exit(f"no {kind} results in {RESULTS}")
    return files[-1]


def judge(name, cur, off, spec, md):
    higher = spec["direction"] == "higher"
    if md is None:
        return "unset", None
    bound = spec.get("max") if higher else spec.get("min")
    saturated = bound is not None and abs(bound - off) <= 2 * md
    thr = off if saturated else (off + md if higher else off - md)
    if saturated:
        return thr, (cur >= thr if higher else cur <= thr)
    return thr, (cur > thr if higher else cur < thr)


def cmd_report(a):
    manifest = load_manifest()
    time_path = Path(a.time) if a.time else latest("time")
    qual_path = Path(a.quality) if a.quality else latest("quality")
    tdata, qdata = json.loads(time_path.read_text()), json.loads(qual_path.read_text())
    stripped = BUILD / "public.stripped.o"
    stripped_bytes = stripped.stat().st_size if stripped.exists() else None
    cases = {c["id"]: c for c in manifest["cases"]}
    report, rows = {}, []
    for cid, c in cases.items():
        q, t = qdata["cases"].get(cid), tdata["cases"].get(cid)
        e = {"target_bitrate_bps": c["bitrate"]}
        if t:
            e.update(median_ms_official=t["median_ms_per_clip"]["official"], median_ms_current=t["median_ms_per_clip"]["current"],
                     speed_margin_pct=t["speed_margin_pct"])
        if q:
            off, cur = q["official"], q["current"]
            e["metrics"] = {}
            all_pass, unset = True, False
            for name, spec in manifest["metrics"].items():
                thr, ok = judge(name, cur[name], off[name], spec, c.get("min_delta", {}).get(name))
                e["metrics"][name] = {"current": cur[name], "official": off[name], "threshold": thr, "pass": ok}
                if ok is None:
                    unset = True
                elif not ok:
                    all_pass = False
            e["quality_pass"] = "unset" if unset and all_pass else all_pass
            target = c["bitrate"]
            floor = 0.99 * min(target, off["actual_bitrate_bps"])
            ceil = float("inf") if c["vbr"] else target
            e.update(official_actual_bps=off["actual_bitrate_bps"], current_actual_bps=cur["actual_bitrate_bps"],
                     utilization_official=off["actual_bitrate_bps"] / target, utilization_current=cur["actual_bitrate_bps"] / target,
                     window_floor_bps=floor, window_ceiling_bps=ceil, window_pass=floor <= cur["actual_bitrate_bps"] <= ceil)
        report[cid] = e
        flat = {"case": cid, **{k: v for k, v in e.items() if k != "metrics"}}
        for name, m in e.get("metrics", {}).items():
            flat.update({f"{name}_current": m["current"], f"{name}_official": m["official"], f"{name}_threshold": m["threshold"], f"{name}_pass": m["pass"]})
        rows.append(flat)
    scaling = []
    for lo, hi in manifest["scaling_pairs"]:
        if lo in tdata["cases"] and hi in tdata["cases"]:
            inc = {k: tdata["cases"][hi]["median_ms_per_clip"][k] - tdata["cases"][lo]["median_ms_per_clip"][k] for k in CODECS}
            scaling.append({"pair": [lo, hi], "increase_ms": inc, "pass": inc["current"] < inc["official"]})
    data = {"upstream_pin": manifest["upstream_pin"], "time_file": str(time_path), "quality_file": str(qual_path),
            "stripped_bytes": stripped_bytes, "scaling": scaling, "cases": report}
    md = [f"# bench17 report", f"pin: `{manifest['upstream_pin']}`  stripped size: {stripped_bytes}", "",
          "| case | off ms | cur ms | margin % | quality | rate | util cur | window |", "|---|---|---|---|---|---|---|---|"]
    for cid, e in report.items():
        md.append(f"| {cid} | {e.get('median_ms_official', float('nan')):.2f} | {e.get('median_ms_current', float('nan')):.2f} | "
                  f"{e.get('speed_margin_pct', float('nan')):+.2f} | {e.get('quality_pass')} | {e.get('current_actual_bps', 0):.0f} / {e['target_bitrate_bps']} | "
                  f"{e.get('utilization_current', 0):.3f} | {e.get('window_pass')} |")
    md += ["", "## metrics (current / official / threshold / pass)", ""]
    for cid, e in report.items():
        md.append(f"### {cid}")
        for name, m in e.get("metrics", {}).items():
            thr = m["threshold"] if isinstance(m["threshold"], str) else f"{m['threshold']:.6g}"
            md.append(f"- {name}: {m['current']:.6g} / {m['official']:.6g} / {thr} / {m['pass']}")
    md += ["", "## scaling", ""] + [f"- {s['pair']}: official +{s['increase_ms']['official']:.2f} ms, current +{s['increase_ms']['current']:.2f} ms, pass={s['pass']}" for s in scaling]
    write_outputs("report", data, rows, "\n".join(md) + "\n")


def cmd_matched(a):
    manifest = load_manifest()
    cases = select(manifest, a.cases)
    inputs = Path(a.inputs)
    verify_inputs(cases, inputs)
    out = {}
    for c in cases:
        off = invoke("quality", "official", c, inputs, a.exe)
        goal, request, best = off["actual_bitrate_bps"], c["bitrate"], None
        for _ in range(8):
            cur = invoke("quality", "current", dict(c, bitrate=request), inputs, a.exe)
            err = cur["actual_bitrate_bps"] / goal - 1
            if best is None or abs(err) < abs(best[2]):
                best = (request, cur, err)
            if abs(err) <= a.tolerance:
                break
            request = max(500, round(request / (1 + err)))
        out[c["id"]] = {"official": off, "current": best[1], "current_request_bps": best[0], "rate_error": best[2]}
        print(f"{c['id']}: request={best[0]} rate_error={best[2]:+.4f}")
    write_outputs("matched", {"upstream_pin": manifest["upstream_pin"], "tolerance": a.tolerance, "cases": out}, [])


def below_fields(case, metrics):
    out = {}
    for name, spec in metrics.items():
        signed = case["current"][name] - case["official"][name]
        signed = signed if spec["direction"] == "higher" else -signed
        if signed < 0:
            out[name] = signed
    return out


def cmd_regress(a):
    manifest = load_manifest()
    metrics = manifest["metrics"]
    deltas = {c["id"]: c["min_delta"] for c in manifest["cases"]}
    base = json.loads(Path(a.base).read_text())["cases"]
    cand = json.loads(Path(a.cand).read_text())["cases"]
    newly, worse, rate = {}, {}, {}
    for cid, c in cand.items():
        b = below_fields(base[cid], metrics)
        for name, v in below_fields(c, metrics).items():
            moved = v - b.get(name, 0.0)
            if moved > -deltas[cid][name]:
                continue
            if name not in b:
                newly.setdefault(cid, {})[name] = v
            else:
                worse.setdefault(cid, {})[name] = (b[name], v)
        if abs(c["rate_error"]) > a.tolerance:
            rate[cid] = c["rate_error"]
    nb = sum(len(below_fields(c, metrics)) for c in base.values())
    nc = sum(len(below_fields(c, metrics)) for c in cand.values())
    print(json.dumps({"below_fields_base": nb, "below_fields_cand": nc, "newly_below": newly, "worsened_below": worse, "unmatched_rate": rate}, indent=1))
    sys.exit(1 if newly or worse or rate else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--inputs", default=str(INPUTS))
    p.add_argument("--exe", type=Path, default=EXE)
    sub = p.add_subparsers(dest="cmd", required=True)
    q = sub.add_parser("quality")
    q.add_argument("--cases")
    q.add_argument("--official-reps", type=int, default=3)
    q.set_defaults(fn=cmd_quality)
    t = sub.add_parser("time")
    t.add_argument("--cases")
    t.add_argument("--reps", type=int, default=15)
    t.add_argument("--cpu", type=int, default=2)
    t.add_argument("--batch", type=int, default=0, help="encoders per run; default 8 for stereo clips, 30 for mono (GetThreadTimes ticks at 15.6 ms)")
    t.set_defaults(fn=cmd_time)
    r = sub.add_parser("report")
    r.add_argument("--time")
    r.add_argument("--quality")
    r.set_defaults(fn=cmd_report)
    m = sub.add_parser("matched")
    m.add_argument("--cases")
    m.add_argument("--tolerance", type=float, default=0.005)
    m.set_defaults(fn=cmd_matched)
    g = sub.add_parser("regress")
    g.add_argument("--base", required=True)
    g.add_argument("--cand", required=True)
    g.add_argument("--tolerance", type=float, default=0.005)
    g.set_defaults(fn=cmd_regress)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
