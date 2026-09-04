#!/usr/bin/env python3
"""One-shot cold session: run the MOMENT the chilled board is plugged in.

Sequence (no waiting, coldest-first):
  1. BQ fuel-gauge temp -> log (the battery chills with the panel)
  2. chart paint + scan at the coldest point
  3. repeat chart every ~2 min while warming, logging temps, until the gauge
     reaches cutoff (default 20°C) or max iterations.

The board must already be flashed with wavecal (it is — flashed warm).

Usage: python3 coldrun.py [--cutoff 20] [--max-iters 10]
"""

import argparse, datetime, os, re, sys, time

import rig


def bq_temp(s):
    m = re.search(r"BQTEMP ([-\d.]+)", rig.command(s, "J", terminator=("BQTEMP", "ERR"))[-1])
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cutoff", type=float, default=20.0)
    ap.add_argument("--max-iters", type=int, default=10)
    ap.add_argument("--port")
    a = ap.parse_args()

    s = rig.open_serial(a.port)
    transform = None
    for it in range(1, a.max_iters + 1):
        bq = bq_temp(s)
        print(f"[cold iter {it}] BQ={bq}°C", flush=True)
        rig.command(s, "P", terminator=("DONE",))
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        png = os.path.join(rig.RESULTS_DIR, f"{stamp}_cold{it}_T{bq}.png")
        os.makedirs(rig.RESULTS_DIR, exist_ok=True)
        rig.scan(png)
        try:
            out, transform = rig.measure(png, None)
            rig.log_result(f"cold{it}", bq, out)
        except SystemExit as e:
            print(f"   measurement failed ({e}); scan kept for later analysis", flush=True)
        if bq is not None and bq >= a.cutoff and it > 1:
            print(f"Cutoff {a.cutoff}°C reached — cold window over.", flush=True)
            break
        time.sleep(60)
    print("Cold run complete.", flush=True)


if __name__ == "__main__":
    main()
