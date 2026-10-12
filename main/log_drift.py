#!/usr/bin/env python3
"""Analyze a Tera Term capture and report the clock correction.

PRINTLOG dumps drift.log. With local timestamps it looks like:

    2026-10-06 09:00:01.123 SYNC 1000 2026-10-01 09:00:00
    2026-10-06 09:00:01.200 DRIFT 432000000 2026-10-06 09:00:01 25.1

A long capture of the live display is also accepted. Those lines look like:

    [2026-10-08 09:04:36.984] [TopLine] 09:04 10/08 | 27C | 62%

Boot lines containing 最終採用時間 start a new powered segment, so time
spent off is not treated as drift.
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


TOP_LINE = re.compile(
    r"\[(?P<pc>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\]\s*"
    r"\[TopLine\]\s+(?P<hh>\d{2}):(?P<mm>\d{2})\s+(?P<mo>\d{2})/(?P<dd>\d{2})"
)
BOOT_LINE = re.compile(
    r"\[(?P<pc>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\]\s*"
    r"最終採用時間:\s*(?P<dev>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})"
)


def read_text(path):
    raw = open(path, "rb").read()
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp950", errors="replace")


def fit_scale(pairs):
    count = len(pairs)
    sum_x = sum_y = sum_xx = sum_xy = 0.0
    for device_s, pc_s in pairs:
        sum_x += device_s
        sum_y += pc_s
        sum_xx += device_s * device_s
        sum_xy += device_s * pc_s
    denom = count * sum_xx - sum_x * sum_x
    if denom == 0:
        return None
    return (count * sum_xy - sum_x * sum_y) / denom


def analyze_topline(text):
    marks = []
    for raw in text.splitlines():
        boot = BOOT_LINE.search(raw)
        if boot:
            marks.append(("boot", parse_pc(boot.group("pc")), parse_pc(boot.group("dev"))))
            continue
        top = TOP_LINE.search(raw)
        if not top:
            continue
        pc = parse_pc(top.group("pc"))
        device = datetime(
            pc.year, int(top.group("mo")), int(top.group("dd")),
            int(top.group("hh")), int(top.group("mm")),
        )
        marks.append(("top", pc, device))

    segments = []
    current = []
    for kind, pc, device in marks:
        if kind == "boot":
            if current:
                segments.append(current)
            current = []
            continue
        if current:
            prev_pc, prev_dev = current[-1]
            device_delta = (device - prev_dev).total_seconds()
            pc_delta = (pc - prev_pc).total_seconds()
            if device_delta < 0 or pc_delta > device_delta + 180:
                segments.append(current)
                current = []
        if not current or device != current[-1][1]:
            current.append((pc, device))
    if current:
        segments.append(current)

    weighted_scale = 0.0
    weighted_span = 0.0
    used = 0
    edge_count = 0
    for edges in segments:
        if len(edges) < 30:
            continue
        origin_pc, origin_dev = edges[0]
        pairs = [
            ((device - origin_dev).total_seconds(), (pc - origin_pc).total_seconds())
            for pc, device in edges
        ]
        span = pairs[-1][0]
        if span < 3600:
            continue
        scale = fit_scale(pairs)
        if scale is None or scale <= 0:
            continue
        weighted_scale += scale * span
        weighted_span += span
        used += 1
        edge_count += len(edges)
        print(f"開機段 {edges[0][1]} -> {edges[-1][1]}，倍率 {scale:.8f}")

    if weighted_span <= 0:
        print("TopLine 的連續紀錄不足，無法計算")
        return 1

    scale = weighted_scale / weighted_span
    per_million = int(round((scale - 1.0) * 1_000_000.0))
    device_seconds = weighted_span
    true_seconds = weighted_span * scale
    print(f"採用 {used} 段開機，裝置經過 {device_seconds:.0f} 秒")
    report_ratio(device_seconds, true_seconds, edge_count)
    print(f"韌體每 1000000 ms 加上 {per_million} ms")
    return 0


def main():
    if len(sys.argv) != 2:
        print(f"用法: python3 {sys.argv[0]} teraterm.log")
        return 1
    text = read_text(sys.argv[1])
    if "DRIFT" in text or "SYNC" in text:
        rows, pc_times = load(sys.argv[1])
        if rows:
            span = (max(pc_times) - min(pc_times)).total_seconds() if len(pc_times) >= 2 else 0
            if span > 6 * 3600:
                analyze_live(rows)
            else:
                analyze_dump(rows, pc_times)
            return 0
    if "[TopLine]" in text:
        return analyze_topline(text)
    print("沒有讀到 DRIFT、SYNC 或 TopLine")
    return 1


if __name__ == "__main__":
    sys.exit(main())
