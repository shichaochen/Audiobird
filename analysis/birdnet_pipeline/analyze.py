#!/usr/bin/env python3
"""
BirdNET batch analysis pipeline.

Scans a folder of audio files (WAV/OGG/MP3/FLAC/M4A), identifies bird species
in every 3-second segment with BirdNET (Cornell Lab, model v2.4, run locally
via birdnetlib + TensorFlow Lite), and writes tidy tables plus charts:

  detections.csv       one row per detection (file, time, species, confidence)
  species_summary.csv  per-species patterns (count, confidence, first/last, peak hour)
  hourly_activity.csv  species x hour-of-day counts (daily rhythm)
  daily_summary.csv    date x species counts (long format)
  files.csv            per-file status (duration, detections, species found)
  species_counts.png   bar chart of detections per species
  hourly_heatmap.png   top species x hour-of-day heatmap
  timeline.png         detections along each file (seconds in, coloured by species)

Optional Audiobird support: pass --log ABIRD_LOG.CSV and the pipeline joins
detections to the device's true recording start times (datetime_local if
present, else datetime_utc) and per-recording lat/lon for BirdNET's
location filter. Without --log, recording start falls back to file mtime
(time_basis column says so, honestly).

Usage:
  ~/workspace/venvs/birdnet311/bin/python analyze.py \
      --input ~/workspace/birdnet_pipeline/demo_data \
      --output ~/workspace/birdnet_pipeline/demo_output \
      --min-conf 0.25
"""

import argparse
import csv
import sys
from collections import Counter, defaultdict
from datetime import datetime, timedelta
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

AUDIO_EXTS = {".wav", ".ogg", ".mp3", ".flac", ".m4a", ".opus"}
SEGMENT_SECONDS = 3.0


def parse_args():
    p = argparse.ArgumentParser(description="Batch bird-sound analysis with BirdNET.")
    p.add_argument("--input", required=True, help="Folder with audio files (scanned recursively).")
    p.add_argument("--output", required=True, help="Folder for CSVs and charts.")
    p.add_argument("--min-conf", type=float, default=0.25,
                   help="Minimum BirdNET confidence to keep (default 0.25).")
    p.add_argument("--lat", type=float, default=None, help="Fallback latitude for species filter.")
    p.add_argument("--lon", type=float, default=None, help="Fallback longitude for species filter.")
    p.add_argument("--date", default=None,
                   help="Fallback recording date YYYY-MM-DD for seasonal filter.")
    p.add_argument("--log", default=None,
                   help="Optional ABIRD_LOG.CSV for true start times and per-file lat/lon.")
    p.add_argument("--no-charts", action="store_true", help="Skip PNG charts.")
    p.add_argument("--top", type=int, default=10, help="Species shown in heatmap (default 10).")
    return p.parse_args()


def parse_dt(text):
    """Parse 'YYYY-MM-DDTHH:MM:SS' (Audiobird log format); None on failure."""
    if not text:
        return None
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d"):
        try:
            return datetime.strptime(text.strip(), fmt)
        except ValueError:
            continue
    return None


def load_abird_log(path):
    """Return {FILENAME_UPPER: {start, basis, lat, lon, loc}} from ABIRD_LOG.CSV."""
    log = {}
    with open(path, newline="", encoding="utf-8-sig") as fh:
        for row in csv.DictReader(fh):
            name = (row.get("file") or "").strip().upper()
            if not name:
                continue
            start, basis = None, "log_utc"
            if row.get("datetime_local"):
                start, basis = parse_dt(row["datetime_local"]), "log_local"
            if start is None and row.get("datetime_utc"):
                start, basis = parse_dt(row["datetime_utc"]), "log_utc"
            if start is None and row.get("unix"):
                try:
                    unix = int(row["unix"])
                    if unix > 0:
                        start, basis = datetime.utcfromtimestamp(unix), "log_utc"
                except ValueError:
                    pass
            try:
                lat = float(row["lat"]) if row.get("lat") else None
                lon = float(row["lon"]) if row.get("lon") else None
            except ValueError:
                lat, lon = None, None
            log[name] = {"start": start, "basis": basis, "lat": lat,
                         "lon": lon, "loc": (row.get("loc_name") or "").strip()}
    return log


def main():
    args = parse_args()
    in_dir, out_dir = Path(args.input), Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    log = load_abird_log(args.log) if args.log else {}
    if args.log:
        print(f"[pipeline] log loaded: {args.log} ({len(log)} rows)")

    fallback_date = parse_dt(args.date) if args.date else None

    files = sorted(f for f in in_dir.rglob("*")
                   if f.is_file() and f.suffix.lower() in AUDIO_EXTS)
    if not files:
        print("[pipeline] no audio files found.")
        return 1
    print(f"[pipeline] {len(files)} audio file(s) found.")

    from birdnetlib import Recording
    from birdnetlib.analyzer import Analyzer

    analyzer = Analyzer()
    print("[pipeline] BirdNET model ready.")

    detections, file_rows = [], []
    for i, fpath in enumerate(files, 1):
        entry = log.get(fpath.name.upper(), {})
        start = entry.get("start")
        basis = entry.get("basis", "mtime")
        if start is None:
            start = datetime.fromtimestamp(fpath.stat().st_mtime)
            basis = "mtime"
        lat = entry.get("lat") if entry.get("lat") is not None else args.lat
        lon = entry.get("lon") if entry.get("lon") is not None else args.lon
        rec_date = start if start else fallback_date
        try:
            rec = Recording(analyzer, str(fpath), lat=lat, lon=lon,
                            date=rec_date, min_conf=args.min_conf)
            rec.analyze()
            found = set()
            for d in rec.detections:
                dt = start + timedelta(seconds=float(d["start_time"]))
                detections.append({
                    "file": fpath.name,
                    "recording_start": start.strftime("%Y-%m-%d %H:%M:%S"),
                    "time_basis": basis,
                    "start_s": float(d["start_time"]),
                    "end_s": float(d["end_time"]),
                    "detect_time": dt.strftime("%Y-%m-%d %H:%M:%S"),
                    "hour": dt.hour,
                    "date": dt.strftime("%Y-%m-%d"),
                    "common_name": d["common_name"],
                    "scientific_name": d["scientific_name"],
                    "confidence": round(float(d["confidence"]), 4),
                    "lat_used": lat if lat is not None else "",
                    "lon_used": lon if lon is not None else "",
                })
                found.add(f"{d['scientific_name']} ({d['common_name']})")
            file_rows.append({"file": fpath.name, "status": "ok",
                              "duration_s": round(float(rec.duration), 1),
                              "detections": len(rec.detections),
                              "species": "; ".join(sorted(found))})
            print(f"[pipeline] {i}/{len(files)} {fpath.name}: "
                  f"{len(rec.detections)} detections, {len(found)} species")
        except Exception as exc:  # keep the batch going; record the failure
            file_rows.append({"file": fpath.name, "status": f"error: {exc}",
                              "duration_s": "", "detections": 0, "species": ""})
            print(f"[pipeline] {i}/{len(files)} {fpath.name}: ERROR {exc}", file=sys.stderr)

    # ---- detections.csv ----
    det_fields = ["file", "recording_start", "time_basis", "start_s", "end_s",
                  "detect_time", "hour", "date", "common_name", "scientific_name",
                  "confidence", "lat_used", "lon_used"]
    with open(out_dir / "detections.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=det_fields)
        w.writeheader()
        w.writerows(detections)

    # ---- files.csv ----
    with open(out_dir / "files.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=["file", "status", "duration_s", "detections", "species"])
        w.writeheader()
        w.writerows(file_rows)

    # ---- species_summary.csv ----
    by_species = defaultdict(list)
    for d in detections:
        by_species[(d["scientific_name"], d["common_name"])].append(d)
    total = len(detections)
    summary = []
    for (sci, com), rows in by_species.items():
        hours = Counter(r["hour"] for r in rows)
        confs = [r["confidence"] for r in rows]
        summary.append({
            "scientific_name": sci, "common_name": com,
            "segments": len(rows), "seconds": len(rows) * SEGMENT_SECONDS,
            "mean_conf": round(sum(confs) / len(confs), 4),
            "max_conf": max(confs),
            "first_detect": min(r["detect_time"] for r in rows),
            "last_detect": max(r["detect_time"] for r in rows),
            "peak_hour": hours.most_common(1)[0][0],
            "peak_hour_segments": hours.most_common(1)[0][1],
            "share": round(len(rows) / total, 4) if total else 0,
        })
    summary.sort(key=lambda r: -r["segments"])
    with open(out_dir / "species_summary.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=list(summary[0].keys()) if summary else
                           ["scientific_name", "common_name", "segments", "seconds",
                            "mean_conf", "max_conf", "first_detect", "last_detect",
                            "peak_hour", "peak_hour_segments", "share"])
        w.writeheader()
        w.writerows(summary)

    # ---- hourly_activity.csv (species x hour matrix) ----
    hours = list(range(24))
    with open(out_dir / "hourly_activity.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["scientific_name", "common_name"] + [f"h{h:02d}" for h in hours] + ["total"])
        for (sci, com), rows in by_species.items():
            c = Counter(r["hour"] for r in rows)
            w.writerow([sci, com] + [c[h] for h in hours] + [len(rows)])

    # ---- daily_summary.csv (long format) ----
    daily = Counter((r["date"], r["scientific_name"], r["common_name"]) for r in detections)
    with open(out_dir / "daily_summary.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["date", "scientific_name", "common_name", "segments"])
        for (date, sci, com), n in sorted(daily.items()):
            w.writerow([date, sci, com, n])

    print(f"[pipeline] total: {total} detections, {len(summary)} species, "
          f"{len(file_rows)} files -> {out_dir}")

    if not args.no_charts and detections:
        make_charts(out_dir, detections, summary, file_rows, args.top)
    elif not detections:
        print("[pipeline] no detections: charts skipped.")
    return 0


def make_charts(out_dir, detections, summary, file_rows, top_n):
    # 1. Species bar chart (top 15)
    top = summary[:15]
    plt.figure(figsize=(10, max(4, 0.55 * len(top))))
    names = [f"{r['common_name']} ({r['scientific_name']})" for r in reversed(top)]
    vals = [r["segments"] for r in reversed(top)]
    plt.barh(names, vals, color="#2e7d32")
    for j, v in enumerate(vals):
        plt.text(v + 0.1, j, str(v), va="center", fontsize=9)
    plt.xlabel("Detection segments (3 s each)")
    plt.title("Detections per species")
    plt.tight_layout()
    plt.savefig(out_dir / "species_counts.png", dpi=150)
    plt.close()

    # 2. Hourly heatmap (top species x hour)
    top_sp = [(r["scientific_name"], r["common_name"]) for r in summary[:top_n]]
    mat = np.zeros((len(top_sp), 24), dtype=int)
    for d in detections:
        key = (d["scientific_name"], d["common_name"])
        if key in top_sp:
            mat[top_sp.index(key), d["hour"]] += 1
    plt.figure(figsize=(12, max(4, 0.6 * len(top_sp))))
    plt.imshow(mat, aspect="auto", cmap="YlGn")
    plt.colorbar(label="segments")
    plt.xticks(range(24), [f"{h:02d}" for h in range(24)])
    plt.yticks(range(len(top_sp)), [c for _, c in top_sp])
    plt.xlabel("Hour of day (recording time basis, see detections.csv)")
    plt.title("When each species calls: hour-of-day activity")
    for i in range(len(top_sp)):
        for h in range(24):
            if mat[i, h]:
                plt.text(h, i, str(mat[i, h]), ha="center", va="center", fontsize=7)
    plt.tight_layout()
    plt.savefig(out_dir / "hourly_heatmap.png", dpi=150)
    plt.close()

    # 3. Timeline: one row per file, ticks coloured by species
    ok_files = [r["file"] for r in file_rows if r["status"] == "ok"]
    species = sorted({(d["scientific_name"], d["common_name"]) for d in detections})
    cmap = plt.get_cmap("tab20")
    color = {sp: cmap(i % 20) for i, sp in enumerate(species)}
    plt.figure(figsize=(12, max(3.5, 0.6 * len(ok_files))))
    for yi, fname in enumerate(ok_files):
        for d in [x for x in detections if x["file"] == fname]:
            key = (d["scientific_name"], d["common_name"])
            plt.plot([d["start_s"], d["end_s"]], [yi, yi], color=color[key],
                     linewidth=6, solid_capstyle="butt")
    plt.yticks(range(len(ok_files)), ok_files)
    plt.xlabel("Seconds into file")
    plt.title("Detection timeline per file")
    handles = [plt.Line2D([0], [0], color=color[(sci, com)], lw=6, label=com)
               for sci, com in species]
    plt.legend(handles=handles, loc="upper right", fontsize=8, framealpha=0.9)
    plt.tight_layout()
    plt.savefig(out_dir / "timeline.png", dpi=150)
    plt.close()
    print("[pipeline] charts: species_counts.png, hourly_heatmap.png, timeline.png")


if __name__ == "__main__":
    sys.exit(main())
