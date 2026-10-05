#!/usr/bin/env python3
"""Analyze a Tera Term capture of PRINTLOG.

The board stores one line per minute on the SD card. PRINTLOG prints that
file. Turn on Tera Term's local timestamp so the dump looks like:

    2026-10-06 09:00:01.123 SYNC 1000 2026-10-01 09:00:00
    2026-10-06 09:00:01.200 DRIFT 432000000 2026-10-06 09:00:01 25.1

SYNC is written when SETTIME matches the PC clock. The script treats that
device time as the real start, and the newest PC timestamp in the file as
the real end.
"""

import re
import sys
from datetime import datetime

LINE = re.compile(
    r"^(?:(?P<pc>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\s+)?"
    r"(?P<kind>DRIFT|SYNC)\s+(?P<ms>\d+)\s+"
    r"(?P<date>\d{4}-\d{2}-\d{2})\s+(?P<time>\d{2}:\d{2}:\d{2})"
)

PC_ONLY = re.compile(
    r"^(?P<pc>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\s+\S"
)


def parse_device(date, clock):
    return datetime.strptime(date + " " + clock, "%Y-%m-%d %H:%M:%S")


def parse_pc(text):
    if "." in text:
        return datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f")
    return datetime.strptime(text, "%Y-%m-%d %H:%M:%S")


def load(path):
    rows = []
    pc_times = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line:
                continue
            match = LINE.search(line)
            if match:
                pc = parse_pc(match.group("pc")) if match.group("pc") else None
                if pc:
                    pc_times.append(pc)
                rows.append({
                    "kind": match.group("kind"),
                    "ms": int(match.group("ms")),
                    "device": parse_device(match.group("date"), match.group("time")),
                    "pc": pc,
                })
                continue
            stamped = PC_ONLY.search(line)
            if stamped:
                pc_times.append(parse_pc(stamped.group("pc")))
    return rows, pc_times


def report_ratio(device_seconds, true_seconds, samples):
    if true_seconds <= 0 or device_seconds <= 0:
        print("時間區間太短，無法計算")
        return
    ppm = (device_seconds / true_seconds - 1.0) * 1_000_000.0
    per_day = ppm * 86400.0 / 1_000_000.0
    scale = true_seconds / device_seconds
    direction = "偏快" if ppm > 0 else "偏慢"
    print(f"樣本 {samples} 筆")
    print(f"板子經過 {device_seconds:.1f} 秒，實際經過 {true_seconds:.1f} 秒")
    print(f"偏移 {ppm:+.1f} ppm，{direction}，約 {per_day:+.2f} 秒/天")
    print(f"修正時把經過的 millis 乘上 {scale:.8f}")


def analyze_dump(rows, pc_times):
    syncs = [i for i, row in enumerate(rows) if row["kind"] == "SYNC"]
    if not syncs:
        print("找不到 SYNC。開始記錄前請先送一次 SETTIME，讓板子時間和電腦對齊。")
        return
    if not pc_times:
        print("檔案裡沒有電腦時間。Tera Term 記錄時要勾選 Timestamp，類型選 Local Time。")
        return

    start_i = syncs[-1]
    start = rows[start_i]
    drifts = [row for row in rows[start_i + 1:] if row["kind"] == "DRIFT"]
    if not drifts:
        print("最後一次 SYNC 之後沒有 DRIFT 紀錄")
        return

    gaps = 0
    previous = start["device"]
    for row in drifts:
        delta = (row["device"] - previous).total_seconds()
        if delta > 180:
            gaps += 1
        if delta >= 0:
            previous = row["device"]

    device_seconds = (drifts[-1]["device"] - start["device"]).total_seconds()
    true_seconds = (max(pc_times) - start["device"]).total_seconds()
    print(f"對時起點 {start['device']}")
    print(f"最後一筆 {drifts[-1]['device']}")
    print(f"印出 log 的電腦時間 {max(pc_times)}")
    if gaps:
        print(f"有 {gaps} 段超過 3 分鐘沒有紀錄。這段若是關機，算出來的偏移會把關機時間算進去。")
    report_ratio(device_seconds, true_seconds, len(drifts))


def analyze_live(rows):
    segments = []
    current = []
    last_ms = None
    for row in rows:
        if row["pc"] is None or row["kind"] != "DRIFT":
            continue
        if last_ms is not None and row["ms"] < last_ms:
            if len(current) >= 2:
                segments.append(current)
            current = []
        current.append(row)
        last_ms = row["ms"]
    if len(current) >= 2:
        segments.append(current)
    if not segments:
        print("有電腦時間，但連續樣本不足")
        return

    best = max(segments, key=lambda seg: (seg[-1]["pc"] - seg[0]["pc"]).total_seconds())
    device_seconds = (best[-1]["ms"] - best[0]["ms"]) / 1000.0
    true_seconds = (best[-1]["pc"] - best[0]["pc"]).total_seconds()
    print("每一行都有電腦時間，改用 millis 對電腦時間。")
    print(f"使用最長的一段：{best[0]['pc']} -> {best[-1]['pc']}")
    report_ratio(device_seconds, true_seconds, len(best))


def main():
    if len(sys.argv) != 2:
        print(f"用法: python3 {sys.argv[0]} teraterm.log")
        return 1
    rows, pc_times = load(sys.argv[1])
    if not rows:
        print("沒有讀到 DRIFT 或 SYNC")
        return 1
    span = (max(pc_times) - min(pc_times)).total_seconds() if len(pc_times) >= 2 else 0
    if span > 6 * 3600:
        analyze_live(rows)
    else:
        analyze_dump(rows, pc_times)
    return 0


if __name__ == "__main__":
    sys.exit(main())
