#!/usr/bin/env python3
"""Cross-check the sensor log against the EMSC catalog and keep the result.

Writes, next to the log:
  events.csv   one row per sensor event and per catalog quake in the region,
               with what the sensor measured when the S wave should arrive
  labels.csv   your own notes (time,label), e.g. "09:54:05,someone sat down";
               created if missing, merged into events.csv on every run
  events/      raw samples from 30 s before to 90 s after each row

Usage: SEISMO_HOME=8.98,-79.52 python3 tools/xcheck.py logs/pga_2026-10-10.csv
"""
import csv, json, math, os, sys, urllib.request
from datetime import datetime, timedelta, timezone

# Where the sensor is, as "lat,lon" in SEISMO_HOME (default: Panama City)
HOME = tuple(float(x) for x in os.environ.get("SEISMO_HOME", "8.98,-79.52").split(","))
REGION = (5, 11, -84, -76)     # lat min/max, lon min/max
LOCAL = timezone(timedelta(hours=-5))
S_KMS = 3.5                    # S wave speed for the expected arrival
WEAK = 0.006                   # same as SUSTAIN_LEVEL in src/config.h


def km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * 6371 * math.asin(math.sqrt(h))


def load_log(path):
    """Samples (t, pga, row) and sensor events from the [trigger]/[event] lines."""
    samples, events = [], []
    dynamic = False  # before the #FMT marker, column 5 is the drifting raw pga
    for line in open(path):
        p = line.rstrip("\n").split(",")
        if p[0] == "#FMT":
            dynamic = True
            continue
        if p[0] == "#LOG" and len(p) >= 3:
            msg = ",".join(p[2:])
            if msg.startswith("[trigger]"):
                t = datetime.fromisoformat(p[1]).replace(tzinfo=LOCAL).timestamp()
                if "strong" in msg or "pga threshold" in msg:
                    level = "rojo"
                else:
                    level = "azul"
                if "escalated" not in msg:
                    events.append({"t": t, "level": level})
                elif events:
                    events[-1]["level"] = "rojo"
            continue
        if p[0].startswith("#") or len(p) < 6:
            continue
        try:
            t = datetime.fromisoformat(p[0]).replace(tzinfo=LOCAL).timestamp()
            samples.append((t, float(p[5]), dynamic, line.rstrip("\n")))
        except ValueError:
            pass
    return samples, events


def window(samples, times, t0, t1):
    import bisect
    return samples[bisect.bisect_left(times, t0):bisect.bisect_right(times, t1)]


def fetch_emsc(t0, t1):
    fmt = "%Y-%m-%dT%H:%M:%S"
    url = ("https://www.seismicportal.eu/fdsnws/event/1/query?format=json&minmag=2"
           f"&start={datetime.fromtimestamp(t0, timezone.utc).strftime(fmt)}"
           f"&end={datetime.fromtimestamp(t1, timezone.utc).strftime(fmt)}"
           f"&minlat={REGION[0]}&maxlat={REGION[1]}&minlon={REGION[2]}&maxlon={REGION[3]}")
    with urllib.request.urlopen(url, timeout=30) as r:
        if r.status == 204:
            return []
        return json.load(r)["features"]


def main():
    log = sys.argv[1]
    out_dir = os.path.dirname(os.path.abspath(log))
    samples, events = load_log(log)
    times = [s[0] for s in samples]
    labels_path = os.path.join(out_dir, "labels.csv")
    if not os.path.exists(labels_path):
        with open(labels_path, "w") as f:
            f.write("time,label\n")
    labels = {}
    for row in csv.DictReader(open(labels_path)):
        labels[row["time"].strip()] = row["label"].strip()
    day = os.path.basename(log)[4:14]  # pga_YYYY-MM-DD.csv

    def label_for(t):
        for k, v in labels.items():
            lt = datetime.fromisoformat(f"{day}T{k}").replace(tzinfo=LOCAL).timestamp()
            if abs(lt - t) <= 60:
                return v
        return ""

    quakes = fetch_emsc(times[0], times[-1])
    rows = []
    for q in quakes:
        p = q["properties"]
        t = datetime.fromisoformat(p["time"].replace("Z", "+00:00")).timestamp()
        d = km(HOME[0], HOME[1], p["lat"], p["lon"])
        s_arr = t + d / S_KMS
        w = [x[1] for x in window(samples, times, s_arr - 15, s_arr + 45)]
        bg = [x[1] for x in window(samples, times, t - 180, t - 10)]
        if not w:
            continue
        near = [e for e in events if s_arr - 30 <= e["t"] <= s_arr + 60]
        rows.append({
            "time_local": datetime.fromtimestamp(t, LOCAL).strftime("%Y-%m-%d %H:%M:%S"),
            "kind": "catalogo", "mag": p["mag"], "dist_km": round(d),
            "depth_km": p["depth"], "region": p["flynn_region"],
            "s_arrival": datetime.fromtimestamp(s_arr, LOCAL).strftime("%H:%M:%S"),
            "sensor_level": near[0]["level"] if near else "",
            "peak_g": round(max(w), 4), "bg_max_g": round(max(bg), 4) if bg else "",
            "n_over_weak": sum(v >= WEAK for v in w),
            "label": label_for(t), "_t": s_arr,
        })
    for e in events:
        matched = [r for r in rows if r["sensor_level"] and abs(r["_t"] - e["t"]) <= 60]
        w = [x[1] for x in window(samples, times, e["t"] - 10, e["t"] + 20)]
        rows.append({
            "time_local": datetime.fromtimestamp(e["t"], LOCAL).strftime("%Y-%m-%d %H:%M:%S"),
            "kind": "sensor", "mag": matched[0]["mag"] if matched else "",
            "dist_km": matched[0]["dist_km"] if matched else "",
            "depth_km": "", "region": matched[0]["region"] if matched else "sin sismo en catalogo",
            "s_arrival": matched[0]["s_arrival"] if matched else "",
            "sensor_level": e["level"], "peak_g": round(max(w), 4) if w else "",
            "bg_max_g": "", "n_over_weak": sum(v >= WEAK for v in w),
            "label": label_for(e["t"]), "_t": e["t"],
        })
    rows.sort(key=lambda r: r["_t"])

    os.makedirs(os.path.join(out_dir, "events"), exist_ok=True)
    for r in rows:
        if r["kind"] == "catalogo" and not r["sensor_level"] and r["peak_g"] < WEAK:
            continue  # nothing to look at
        name = r["time_local"].replace(" ", "_").replace(":", "") + f"_{r['kind']}.csv"
        with open(os.path.join(out_dir, "events", name), "w") as f:
            f.write("time,millis,ax,ay,az,pga,pga_raw\n")
            for s in window(samples, times, r["_t"] - 30, r["_t"] + 90):
                f.write(s[3] + "\n")

    fields = [k for k in rows[0] if not k.startswith("_")] if rows else []
    path = os.path.join(out_dir, "events.csv")
    old = []
    if os.path.exists(path):  # keep other days' rows
        old = [r for r in csv.DictReader(open(path)) if not r["time_local"].startswith(day)]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(sorted(old + rows, key=lambda r: r["time_local"]))

    seen = [r for r in rows if r["kind"] == "catalogo" and r["sensor_level"]]
    false = [r for r in rows if r["kind"] == "sensor" and not r["mag"]]
    print(f"{len(quakes)} sismos en catalogo, {len(seen)} detectados por el sensor, "
          f"{len(false)} eventos del sensor sin sismo -> {path}")


if __name__ == "__main__":
    main()
