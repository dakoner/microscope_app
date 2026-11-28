#!/usr/bin/env python3
"""Scan a video, write a CSV report of frame-to-frame pixel offsets, and plot the
pixel-space movement trajectory as a series of line segments.

Sign convention:
    (dx, dy) is the image-feature shift from the previous frame to the current frame:
    a feature at (x, y) in frame i-1 appears at (x + dx, y + dy) in frame i.
    +X is right, +Y is down (standard image pixel coordinates).

Usage:
    .venv/Scripts/python.exe scripts/video_frame_offsets.py
    .venv/Scripts/python.exe scripts/video_frame_offsets.py path/to/video.mkv
    .venv/Scripts/python.exe scripts/video_frame_offsets.py --from-csv videos/recording_1795837918_offsets.csv
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

DEFAULT_VIDEO = "C:/Users/davidek/microscope_app/videos/recording_1795837918.mkv"


@dataclass
class FrameOffset:
    frame: int
    prev_frame: int
    time_s: float
    dx: float
    dy: float
    dist: float
    cum_x: float
    cum_y: float
    response: float


def _to_gray_float(frame: np.ndarray, scale: float, highpass: bool) -> np.ndarray:
    if frame.ndim == 3:
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    else:
        gray = frame
    if scale != 1.0:
        h, w = gray.shape
        new_w = max(16, int(round(w * scale)))
        new_h = max(16, int(round(h * scale)))
        gray = cv2.resize(gray, (new_w, new_h), interpolation=cv2.INTER_AREA)
    f = gray.astype(np.float32)
    if highpass:
        h, w = f.shape
        small = cv2.resize(f, (max(1, w // 4), max(1, h // 4)), interpolation=cv2.INTER_AREA)
        low_small = cv2.GaussianBlur(small, (0, 0), 25.0 * scale / 4.0)
        low = cv2.resize(low_small, (w, h), interpolation=cv2.INTER_LINEAR)
        f = f - low
    return f


def scan_video(
    video_path: Path,
    start: int = 0,
    end: int | None = None,
    step: int = 1,
    scale: float = 1.0,
    highpass: bool = False,
) -> tuple[list[FrameOffset], dict]:
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"Failed to open video: {video_path}")

    meta = {
        "video": str(video_path),
        "width": int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
        "height": int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
        "fps": float(cap.get(cv2.CAP_PROP_FPS)) or 0.0,
        "reported_frames": int(cap.get(cv2.CAP_PROP_FRAME_COUNT)),
    }

    if start > 0:
        cap.set(cv2.CAP_PROP_POS_FRAMES, start)

    records: list[FrameOffset] = []
    prev_gray: np.ndarray | None = None
    prev_idx: int | None = None
    hann: np.ndarray | None = None

    cum_x = 0.0
    cum_y = 0.0
    frame_idx = start

    try:
        while True:
            if end is not None and frame_idx >= end:
                break
            ret, frame = cap.read()
            if not ret:
                break

            if (frame_idx - start) % step == 0:
                gray = _to_gray_float(frame, scale, highpass)
                if hann is None:
                    hann = cv2.createHanningWindow((gray.shape[1], gray.shape[0]), cv2.CV_32F)

                fps = meta["fps"]
                time_s = (frame_idx / fps) if fps > 0.0 else 0.0

                if prev_gray is None:
                    dx, dy, dist, resp = 0.0, 0.0, 0.0, 1.0
                    p_idx = frame_idx
                else:
                    (sdx, sdy), resp = cv2.phaseCorrelate(prev_gray, gray, hann)
                    dx = float(sdx) / scale
                    dy = float(sdy) / scale
                    dist = math.hypot(dx, dy)
                    cum_x += dx
                    cum_y += dy
                    p_idx = prev_idx if prev_idx is not None else frame_idx

                records.append(
                    FrameOffset(
                        frame=frame_idx,
                        prev_frame=p_idx,
                        time_s=time_s,
                        dx=dx,
                        dy=dy,
                        dist=dist,
                        cum_x=cum_x,
                        cum_y=cum_y,
                        response=float(resp),
                    )
                )
                prev_gray = gray
                prev_idx = frame_idx

            frame_idx += 1
    finally:
        cap.release()

    return records, meta


def write_csv_report(csv_path: Path, records: list[FrameOffset]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            ["frame", "prev_frame", "time_s", "dx_px", "dy_px", "dist_px", "cum_x_px", "cum_y_px", "response"]
        )
        for r in records:
            writer.writerow(
                [
                    r.frame,
                    r.prev_frame,
                    f"{r.time_s:.4f}",
                    f"{r.dx:.4f}",
                    f"{r.dy:.4f}",
                    f"{r.dist:.4f}",
                    f"{r.cum_x:.4f}",
                    f"{r.cum_y:.4f}",
                    f"{r.response:.4f}",
                ]
            )


def load_csv_report(csv_path: Path) -> list[FrameOffset]:
    records: list[FrameOffset] = []
    with csv_path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            records.append(
                FrameOffset(
                    frame=int(row["frame"]),
                    prev_frame=int(row["prev_frame"]),
                    time_s=float(row["time_s"]),
                    dx=float(row["dx_px"]),
                    dy=float(row["dy_px"]),
                    dist=float(row["dist_px"]),
                    cum_x=float(row["cum_x_px"]),
                    cum_y=float(row["cum_y_px"]),
                    response=float(row["response"]),
                )
            )
    return records


def _nice_step(span: float, target_ticks: int = 8) -> float:
    if span <= 0 or not math.isfinite(span):
        return 1.0
    raw = span / max(1, target_ticks)
    mag = 10.0 ** math.floor(math.log10(raw))
    norm = raw / mag
    for nice in (1.0, 2.0, 2.5, 5.0, 10.0):
        if norm <= nice:
            return nice * mag
    return 10.0 * mag


def _ticks_in_range(vmin: float, vmax: float, step: float) -> list[float]:
    if step <= 0 or vmax <= vmin:
        return [vmin]
    first = math.ceil(vmin / step - 1e-9) * step
    ticks: list[float] = []
    val = first
    while val <= vmax + step * 1e-6 and len(ticks) < 200:
        if abs(val) < step * 1e-9:
            val = 0.0
        ticks.append(val)
        val += step
    return ticks


def _fmt_tick(v: float, step: float) -> str:
    if step >= 1.0:
        return f"{v:.0f}"
    if step >= 0.1:
        return f"{v:.1f}"
    return f"{v:.2f}"


def _colormap_bgr(t: float) -> tuple[int, int, int]:
    """Smooth perceptual colormap (blue -> cyan -> green -> amber -> red) in BGR."""
    t = max(0.0, min(1.0, t))
    # Control points in RGB
    stops = [
        (0.00, (37, 99, 235)),   # royal blue
        (0.28, (6, 182, 212)),   # cyan
        (0.55, (22, 163, 74)),   # green
        (0.78, (234, 138, 12)),  # amber
        (1.00, (220, 38, 38)),   # crimson
    ]
    for i in range(len(stops) - 1):
        t0, c0 = stops[i]
        t1, c1 = stops[i + 1]
        if t <= t1:
            u = (t - t0) / max(1e-9, t1 - t0)
            r = int(round(c0[0] + u * (c1[0] - c0[0])))
            g = int(round(c0[1] + u * (c1[1] - c0[1])))
            b = int(round(c0[2] + u * (c1[2] - c0[2])))
            return (b, g, r)
    r, g, b = stops[-1][1]
    return (b, g, r)


def _put_text_halo(
    img: np.ndarray,
    text: str,
    org: tuple[int, int],
    scale: float,
    color: tuple[int, int, int],
    thickness: int = 1,
    halo_color: tuple[int, int, int] = (255, 255, 255),
) -> None:
    font = cv2.FONT_HERSHEY_SIMPLEX
    cv2.putText(img, text, org, font, scale, halo_color, thickness + 2, cv2.LINE_AA)
    cv2.putText(img, text, org, font, scale, color, thickness, cv2.LINE_AA)


def _draw_trajectory_panel(
    img: np.ndarray,
    rect: tuple[int, int, int, int],
    records: list[FrameOffset],
    equal_aspect: bool,
    title: str,
) -> None:
    x0, y0, pw, ph = rect
    x1, y1 = x0 + pw, y0 + ph

    # Panel background & border
    cv2.rectangle(img, (x0, y0), (x1, y1), (250, 251, 253), -1)
    cv2.rectangle(img, (x0, y0), (x1, y1), (190, 196, 206), 1)

    # Title
    cv2.putText(
        img, title, (x0 + 6, y0 - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.58, (30, 35, 45), 1, cv2.LINE_AA
    )

    xs = np.array([r.cum_x for r in records], dtype=np.float64)
    ys = np.array([r.cum_y for r in records], dtype=np.float64)

    xmin, xmax = float(xs.min()), float(xs.max())
    ymin, ymax = float(ys.min()), float(ys.max())
    dx_span = max(xmax - xmin, 1.0)
    dy_span = max(ymax - ymin, 1.0)

    pad_frac = 0.08
    if equal_aspect:
        cx = 0.5 * (xmin + xmax)
        cy = 0.5 * (ymin + ymax)
        sx = (pw * (1.0 - 2.0 * pad_frac)) / dx_span
        sy = (ph * (1.0 - 2.0 * pad_frac)) / dy_span
        scale = min(sx, sy)
        half_w_data = (pw / 2.0) / scale
        half_h_data = (ph / 2.0) / scale
        vx0, vx1 = cx - half_w_data, cx + half_w_data
        vy0, vy1 = cy - half_h_data, cy + half_h_data
    else:
        vx0 = xmin - dx_span * pad_frac
        vx1 = xmax + dx_span * pad_frac
        vy0 = ymin - dy_span * pad_frac
        vy1 = ymax + dy_span * pad_frac

    def to_px(wx: float, wy: float) -> tuple[float, float]:
        px = x0 + (wx - vx0) / (vx1 - vx0) * pw
        # Image coordinates: +Y downward
        py = y0 + (wy - vy0) / (vy1 - vy0) * ph
        return px, py

    # Grid & ticks
    x_step = _nice_step(vx1 - vx0, target_ticks=10)
    y_step = x_step if equal_aspect else _nice_step(vy1 - vy0, target_ticks=5)
    font = cv2.FONT_HERSHEY_SIMPLEX

    for tx in _ticks_in_range(vx0, vx1, x_step):
        px = int(round(to_px(tx, vy0)[0]))
        if x0 <= px <= x1:
            col = (195, 200, 210) if abs(tx) < 1e-9 else (232, 236, 242)
            thick = 1
            cv2.line(img, (px, y0), (px, y1), col, thick)
            cv2.line(img, (px, y1), (px, y1 + 5), (120, 128, 140), 1)
            lbl = _fmt_tick(tx, x_step)
            (tw, th), _ = cv2.getTextSize(lbl, font, 0.44, 1)
            cv2.putText(img, lbl, (px - tw // 2, y1 + 20), font, 0.44, (70, 76, 88), 1, cv2.LINE_AA)

    for ty in _ticks_in_range(vy0, vy1, y_step):
        py = int(round(to_px(vx0, ty)[1]))
        if y0 <= py <= y1:
            col = (195, 200, 210) if abs(ty) < 1e-9 else (232, 236, 242)
            cv2.line(img, (x0, py), (x1, py), col, 1)
            cv2.line(img, (x0 - 5, py), (x0, py), (120, 128, 140), 1)
            lbl = _fmt_tick(ty, y_step)
            (tw, th), _ = cv2.getTextSize(lbl, font, 0.44, 1)
            cv2.putText(img, lbl, (x0 - tw - 10, py + th // 2), font, 0.44, (70, 76, 88), 1, cv2.LINE_AA)

    # Axis labels
    xlbl = "Cumulative X offset (px)"
    (tw, _), _ = cv2.getTextSize(xlbl, font, 0.48, 1)
    cv2.putText(img, xlbl, (x0 + (pw - tw) // 2, y1 + 40), font, 0.48, (50, 56, 66), 1, cv2.LINE_AA)

    cv2.putText(img, "Y (px)", (10, y0 + ph // 2 - 4), font, 0.42, (70, 76, 88), 1, cv2.LINE_AA)
    cv2.putText(img, "+down", (10, y0 + ph // 2 + 13), font, 0.38, (105, 112, 124), 1, cv2.LINE_AA)

    # Draw line segments with subpixel precision (shift=4 -> 1/16 px)
    shift = 4
    scale_s = 1 << shift
    n = len(records)
    denom = max(1, n - 1)

    pts_px = [to_px(r.cum_x, r.cum_y) for r in records]
    for i in range(1, n):
        p0 = (int(round(pts_px[i - 1][0] * scale_s)), int(round(pts_px[i - 1][1] * scale_s)))
        p1 = (int(round(pts_px[i][0] * scale_s)), int(round(pts_px[i][1] * scale_s)))
        col = _colormap_bgr(i / denom)
        cv2.line(img, p0, p1, col, 2, cv2.LINE_AA, shift=shift)

    # Draw subtle direction chevrons along the trajectory
    if n >= 20:
        cum_px_dist = [0.0]
        for i in range(1, n):
            d = math.hypot(pts_px[i][0] - pts_px[i - 1][0], pts_px[i][1] - pts_px[i - 1][1])
            cum_px_dist.append(cum_px_dist[-1] + d)
        total_screen_len = cum_px_dist[-1]
        if total_screen_len > 120.0:
            num_arrows = min(8, max(2, int(total_screen_len // 180)))
            for k in range(1, num_arrows + 1):
                target = total_screen_len * (k / (num_arrows + 1))
                idx = int(np.searchsorted(cum_px_dist, target))
                idx = max(2, min(n - 3, idx))
                # Average direction over a small window for stable arrow angle
                j0 = max(0, idx - 5)
                j1 = min(n - 1, idx + 5)
                vx = pts_px[j1][0] - pts_px[j0][0]
                vy = pts_px[j1][1] - pts_px[j0][1]
                vlen = math.hypot(vx, vy)
                if vlen > 1.0:
                    ux, uy = vx / vlen, vy / vlen
                    ax, ay = pts_px[idx]
                    col = _colormap_bgr(idx / denom)
                    tip = (int(round(ax + ux * 6)), int(round(ay + uy * 6)))
                    tail = (int(round(ax - ux * 6)), int(round(ay - uy * 6)))
                    cv2.arrowedLine(img, tail, tip, col, 2, cv2.LINE_AA, tipLength=0.65)

    # Milestone frame markers (evenly spaced in frame count, avoiding overlap)
    if n >= 10:
        n_marks = 6
        last_label_xy = (-1e9, -1e9)
        for k in range(1, n_marks):
            idx = int(round(k * (n - 1) / n_marks))
            px, py = pts_px[idx]
            if math.hypot(px - last_label_xy[0], py - last_label_xy[1]) < 65:
                continue
            last_label_xy = (px, py)
            col = _colormap_bgr(idx / denom)
            cv2.circle(img, (int(round(px)), int(round(py))), 4, (255, 255, 255), -1, cv2.LINE_AA)
            cv2.circle(img, (int(round(px)), int(round(py))), 4, col, 2, cv2.LINE_AA)
            ly = int(round(py)) - 10 if py - y0 > 28 else int(round(py)) + 18
            _put_text_halo(img, f"f{records[idx].frame}", (int(round(px)) - 16, ly), 0.40, (45, 50, 60), 1)

    # Start & End markers
    sx, sy = int(round(pts_px[0][0])), int(round(pts_px[0][1]))
    ex, ey = int(round(pts_px[-1][0])), int(round(pts_px[-1][1]))
    cv2.circle(img, (sx, sy), 6, (34, 175, 76), -1, cv2.LINE_AA)
    cv2.circle(img, (sx, sy), 6, (20, 80, 35), 1, cv2.LINE_AA)
    _put_text_halo(img, f"Start (f{records[0].frame})", (sx + 9, sy - 6), 0.44, (20, 110, 45), 1)

    cv2.rectangle(img, (ex - 5, ey - 5), (ex + 5, ey + 5), (38, 38, 220), -1)
    cv2.rectangle(img, (ex - 5, ey - 5), (ex + 5, ey + 5), (100, 18, 18), 1)
    end_lbl = f"End (f{records[-1].frame})"
    (etw, _), _ = cv2.getTextSize(end_lbl, font, 0.44, 1)
    elx = ex - etw - 10 if ex + etw + 14 > x1 else ex + 9
    _put_text_halo(img, end_lbl, (elx, ey - 6), 0.44, (175, 25, 25), 1)


def _draw_step_panel(
    img: np.ndarray,
    rect: tuple[int, int, int, int],
    records: list[FrameOffset],
) -> None:
    x0, y0, pw, ph = rect
    x1, y1 = x0 + pw, y0 + ph

    cv2.rectangle(img, (x0, y0), (x1, y1), (250, 251, 253), -1)
    cv2.rectangle(img, (x0, y0), (x1, y1), (190, 196, 206), 1)

    font = cv2.FONT_HERSHEY_SIMPLEX
    cv2.putText(
        img,
        "Per-Frame Offset vs Frame (px/frame)",
        (x0 + 6, y0 - 10),
        font,
        0.58,
        (30, 35, 45),
        1,
        cv2.LINE_AA,
    )

    # Legend in top-right of panel
    cv2.line(img, (x1 - 185, y0 - 15), (x1 - 165, y0 - 15), (220, 105, 30), 2, cv2.LINE_AA)
    cv2.putText(img, "dx", (x1 - 158, y0 - 10), font, 0.46, (40, 46, 56), 1, cv2.LINE_AA)
    cv2.line(img, (x1 - 110, y0 - 15), (x1 - 90, y0 - 15), (30, 130, 235), 2, cv2.LINE_AA)
    cv2.putText(img, "dy", (x1 - 83, y0 - 10), font, 0.46, (40, 46, 56), 1, cv2.LINE_AA)

    frames = np.array([r.frame for r in records], dtype=np.float64)
    dxs = np.array([r.dx for r in records], dtype=np.float64)
    dys = np.array([r.dy for r in records], dtype=np.float64)

    fmin, fmax = float(frames[0]), max(float(frames[-1]), float(frames[0]) + 1.0)
    vmax = max(float(np.max(np.abs(dxs))), float(np.max(np.abs(dys))), 0.5)
    vy0, vy1 = -vmax * 1.12, vmax * 1.12

    def to_px(fr: float, val: float) -> tuple[int, int]:
        px = int(round(x0 + (fr - fmin) / (fmax - fmin) * pw))
        py = int(round(y1 - (val - vy0) / (vy1 - vy0) * ph))
        return px, py

    # Grid
    f_step = _nice_step(fmax - fmin, target_ticks=10)
    for tf in _ticks_in_range(fmin, fmax, f_step):
        px = to_px(tf, 0.0)[0]
        if x0 <= px <= x1:
            cv2.line(img, (px, y0), (px, y1), (232, 236, 242), 1)
            cv2.line(img, (px, y1), (px, y1 + 5), (120, 128, 140), 1)
            lbl = f"{int(round(tf))}"
            (tw, _), _ = cv2.getTextSize(lbl, font, 0.44, 1)
            cv2.putText(img, lbl, (px - tw // 2, y1 + 20), font, 0.44, (70, 76, 88), 1, cv2.LINE_AA)

    v_step = _nice_step(vy1 - vy0, target_ticks=5)
    for tv in _ticks_in_range(vy0, vy1, v_step):
        py = to_px(fmin, tv)[1]
        if y0 <= py <= y1:
            col = (180, 186, 198) if abs(tv) < 1e-9 else (232, 236, 242)
            cv2.line(img, (x0, py), (x1, py), col, 1)
            cv2.line(img, (x0 - 5, py), (x0, py), (120, 128, 140), 1)
            lbl = _fmt_tick(tv, v_step)
            (tw, th), _ = cv2.getTextSize(lbl, font, 0.44, 1)
            cv2.putText(img, lbl, (x0 - tw - 10, py + th // 2), font, 0.44, (70, 76, 88), 1, cv2.LINE_AA)

    xlbl = "Frame index"
    (tw, _), _ = cv2.getTextSize(xlbl, font, 0.48, 1)
    cv2.putText(img, xlbl, (x0 + (pw - tw) // 2, y1 + 40), font, 0.48, (50, 56, 66), 1, cv2.LINE_AA)

    # Plot polylines (dy first, then dx on top)
    pts_dy = np.array([to_px(frames[i], dys[i]) for i in range(len(records))], dtype=np.int32)
    pts_dx = np.array([to_px(frames[i], dxs[i]) for i in range(len(records))], dtype=np.int32)
    if len(pts_dy) >= 2:
        cv2.polylines(img, [pts_dy], False, (30, 130, 235), 1, cv2.LINE_AA)
        cv2.polylines(img, [pts_dx], False, (220, 105, 30), 1, cv2.LINE_AA)


def write_trajectory_png(png_path: Path, records: list[FrameOffset], title_info: str) -> None:
    if not records:
        raise RuntimeError("No frame records to plot.")

    W, H = 1800, 1260
    img = np.full((H, W, 3), 255, dtype=np.uint8)
    font = cv2.FONT_HERSHEY_SIMPLEX

    # Compute summary stats
    steps = [r.dist for r in records[1:]] if len(records) > 1 else [0.0]
    path_len = float(sum(steps))
    max_r = max(records[1:], key=lambda r: r.dist) if len(records) > 1 else records[0]

    # Header
    cv2.putText(img, "Video Frame-to-Frame Pixel Trajectory", (95, 40), font, 0.90, (20, 26, 36), 2, cv2.LINE_AA)
    cv2.putText(img, title_info, (95, 66), font, 0.48, (80, 88, 100), 1, cv2.LINE_AA)
    stats_txt = (
        f"Frames: {len(records)}  |  Net offset: ({records[-1].cum_x:+.2f}, {records[-1].cum_y:+.2f}) px  |  "
        f"Path length: {path_len:.2f} px  |  Max step: {max_r.dist:.2f} px @ frame {max_r.frame}"
    )
    cv2.putText(img, stats_txt, (95, 88), font, 0.46, (55, 64, 78), 1, cv2.LINE_AA)

    # Colorbar in top-right header
    cb_x0, cb_y0, cb_w, cb_h = W - 360, 28, 280, 16
    for i in range(cb_w):
        col = _colormap_bgr(i / max(1, cb_w - 1))
        cv2.line(img, (cb_x0 + i, cb_y0), (cb_x0 + i, cb_y0 + cb_h), col, 1)
    cv2.rectangle(img, (cb_x0, cb_y0), (cb_x0 + cb_w, cb_y0 + cb_h), (140, 148, 160), 1)
    cv2.putText(img, f"Frame {records[0].frame}", (cb_x0, cb_y0 + cb_h + 16), font, 0.40, (70, 76, 88), 1, cv2.LINE_AA)
    end_txt = f"Frame {records[-1].frame}"
    (etw, _), _ = cv2.getTextSize(end_txt, font, 0.40, 1)
    cv2.putText(img, end_txt, (cb_x0 + cb_w - etw, cb_y0 + cb_h + 16), font, 0.40, (70, 76, 88), 1, cv2.LINE_AA)

    # Layout panels
    left_m = 95
    right_m = 55
    panel_w = W - left_m - right_m

    _draw_trajectory_panel(
        img,
        (left_m, 130, panel_w, 340),
        records,
        equal_aspect=True,
        title="Pixel-Space Movement Trajectory - True 1:1 Aspect Ratio (line segments per frame)",
    )
    _draw_trajectory_panel(
        img,
        (left_m, 555, panel_w, 310),
        records,
        equal_aspect=False,
        title="Pixel-Space Movement Trajectory - Auto-Scaled X/Y (detail view)",
    )
    _draw_step_panel(
        img,
        (left_m, 950, panel_w, 240),
        records,
    )

    png_path.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(png_path), img):
        raise RuntimeError(f"Failed to write plot image: {png_path}")


def write_trajectory_svg(svg_path: Path, records: list[FrameOffset], title_info: str) -> None:
    """Write a zoomable vector SVG of the pixel-space line segments with per-frame tooltips."""
    if not records:
        return

    xs = [r.cum_x for r in records]
    ys = [r.cum_y for r in records]
    xmin, xmax = min(xs), max(xs)
    ymin, ymax = min(ys), max(ys)
    dx_span = max(xmax - xmin, 1.0)
    dy_span = max(ymax - ymin, 1.0)

    W, H = 1400, 620
    margin_l, margin_r, margin_t, margin_b = 80, 40, 65, 65
    pw = W - margin_l - margin_r
    ph = H - margin_t - margin_b

    pad = 0.08
    cx = 0.5 * (xmin + xmax)
    cy = 0.5 * (ymin + ymax)
    scale = min((pw * (1.0 - 2 * pad)) / dx_span, (ph * (1.0 - 2 * pad)) / dy_span)
    vx0, vx1 = cx - (pw / 2.0) / scale, cx + (pw / 2.0) / scale
    vy0, vy1 = cy - (ph / 2.0) / scale, cy + (ph / 2.0) / scale

    def sx(wx: float) -> float:
        return margin_l + (wx - vx0) / (vx1 - vx0) * pw

    def sy(wy: float) -> float:
        return margin_t + (wy - vy0) / (vy1 - vy0) * ph

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
        f'style="background:#ffffff;font-family:Segoe UI,Arial,sans-serif">',
        f'<text x="{margin_l}" y="28" font-size="18" font-weight="600" fill="#141a24">'
        f'Pixel-Space Frame Movement Trajectory (1:1 aspect)</text>',
        f'<text x="{margin_l}" y="48" font-size="12" fill="#505864">{title_info}</text>',
        f'<rect x="{margin_l}" y="{margin_t}" width="{pw}" height="{ph}" fill="#fafbfd" stroke="#bec4ce"/>',
    ]

    step = _nice_step(vx1 - vx0, target_ticks=10)
    for tx in _ticks_in_range(vx0, vx1, step):
        px = sx(tx)
        col = "#c3c8d2" if abs(tx) < 1e-9 else "#e8ecf2"
        lines.append(f'<line x1="{px:.2f}" y1="{margin_t}" x2="{px:.2f}" y2="{margin_t + ph}" stroke="{col}"/>')
        lines.append(
            f'<text x="{px:.2f}" y="{margin_t + ph + 18}" text-anchor="middle" font-size="11" fill="#464c58">'
            f'{_fmt_tick(tx, step)}</text>'
        )
    for ty in _ticks_in_range(vy0, vy1, step):
        py = sy(ty)
        col = "#c3c8d2" if abs(ty) < 1e-9 else "#e8ecf2"
        lines.append(f'<line x1="{margin_l}" y1="{py:.2f}" x2="{margin_l + pw}" y2="{py:.2f}" stroke="{col}"/>')
        lines.append(
            f'<text x="{margin_l - 8}" y="{py + 4:.2f}" text-anchor="end" font-size="11" fill="#464c58">'
            f'{_fmt_tick(ty, step)}</text>'
        )

    lines.append(
        f'<text x="{margin_l + pw / 2:.1f}" y="{H - 18}" text-anchor="middle" font-size="12" fill="#323842">'
        f'Cumulative X offset (px)</text>'
    )
    lines.append(
        f'<text x="20" y="{margin_t + ph / 2:.1f}" text-anchor="middle" font-size="12" fill="#323842" '
        f'transform="rotate(-90 20 {margin_t + ph / 2:.1f})">Cumulative Y offset (px, +down)</text>'
    )

    denom = max(1, len(records) - 1)
    for i in range(1, len(records)):
        r0, r1 = records[i - 1], records[i]
        b, g, r = _colormap_bgr(i / denom)
        hex_col = f"#{r:02x}{g:02x}{b:02x}"
        lines.append(
            f'<line x1="{sx(r0.cum_x):.2f}" y1="{sy(r0.cum_y):.2f}" '
            f'x2="{sx(r1.cum_x):.2f}" y2="{sy(r1.cum_y):.2f}" '
            f'stroke="{hex_col}" stroke-width="1.6" stroke-linecap="round">'
            f'<title>Frame {r1.prev_frame}-&gt;{r1.frame} (t={r1.time_s:.3f}s): '
            f'dx={r1.dx:+.3f}px, dy={r1.dy:+.3f}px, cum=({r1.cum_x:+.2f}, {r1.cum_y:+.2f})px</title>'
            f'</line>'
        )

    # Start & end markers
    lines.append(
        f'<circle cx="{sx(records[0].cum_x):.2f}" cy="{sy(records[0].cum_y):.2f}" r="5" '
        f'fill="#22af4c" stroke="#145023" stroke-width="1.2"/>'
    )
    lines.append(
        f'<rect x="{sx(records[-1].cum_x) - 4.5:.2f}" y="{sy(records[-1].cum_y) - 4.5:.2f}" '
        f'width="9" height="9" fill="#dc2626" stroke="#641212" stroke-width="1.2"/>'
    )
    lines.append("</svg>")

    svg_path.parent.mkdir(parents=True, exist_ok=True)
    svg_path.write_text("\n".join(lines), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Scan a video, write a CSV report of per-frame pixel offsets, and render a trajectory plot."
    )
    parser.add_argument(
        "video",
        nargs="?",
        default=DEFAULT_VIDEO,
        help=f"Path to video file (default: {DEFAULT_VIDEO})",
    )
    parser.add_argument(
        "--from-csv",
        type=Path,
        default=None,
        help="Skip video scanning and generate plot(s) from an existing CSV report",
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=None,
        help="Output CSV report path (default: <video_dir>/<video_stem>_offsets.csv)",
    )
    parser.add_argument(
        "--no-csv",
        action="store_true",
        help="Do not write a CSV report file",
    )
    parser.add_argument(
        "--plot",
        type=Path,
        default=None,
        help="Output PNG trajectory plot path (default: <video_dir>/<video_stem>_trajectory.png)",
    )
    parser.add_argument(
        "--no-plot",
        action="store_true",
        help="Do not write a PNG trajectory plot",
    )
    parser.add_argument(
        "--svg",
        type=Path,
        default=None,
        help="Output vector SVG trajectory path (default: <video_dir>/<video_stem>_trajectory.svg)",
    )
    parser.add_argument(
        "--no-svg",
        action="store_true",
        help="Do not write an SVG trajectory plot",
    )
    parser.add_argument(
        "--stdout",
        action="store_true",
        help="Also print every frame's offset row to stdout",
    )
    parser.add_argument(
        "--start",
        type=int,
        default=0,
        help="First frame index to read (default: 0)",
    )
    parser.add_argument(
        "--end",
        type=int,
        default=None,
        help="Stop before this frame index (default: end of video)",
    )
    parser.add_argument(
        "--step",
        type=int,
        default=1,
        help="Frame stride (default: 1, every consecutive frame)",
    )
    parser.add_argument(
        "--scale",
        type=float,
        default=1.0,
        help="Resize factor before phase correlation (reported offsets remain in original pixels; default: 1.0)",
    )
    parser.add_argument(
        "--highpass",
        action="store_true",
        help="Apply high-pass filter to suppress static illumination/vignetting",
    )
    args = parser.parse_args(argv)

    if args.step < 1:
        parser.error("--step must be >= 1")
    if args.scale <= 0.0 or args.scale > 1.0:
        parser.error("--scale must be in (0, 1]")

    t0 = time.perf_counter()

    if args.from_csv is not None:
        if not args.from_csv.exists():
            print(f"Error: CSV file not found: {args.from_csv}", file=sys.stderr)
            return 1
        records = load_csv_report(args.from_csv)
        base_stem = args.from_csv.stem.removesuffix("_offsets")
        base_dir = args.from_csv.parent
        title_info = f"Source CSV: {args.from_csv.name}"
        write_csv = False
    else:
        video_path = Path(args.video)
        if not video_path.exists():
            print(f"Error: video file not found: {video_path}", file=sys.stderr)
            return 1
        records, meta = scan_video(
            video_path,
            start=args.start,
            end=args.end,
            step=args.step,
            scale=args.scale,
            highpass=args.highpass,
        )
        base_stem = video_path.stem
        base_dir = video_path.parent
        title_info = f"{video_path.name}  ({meta['width']}x{meta['height']}, {meta['fps']:.2f} fps)"
        write_csv = not args.no_csv

    if not records:
        print("Error: no frames were read.", file=sys.stderr)
        return 1

    if args.stdout:
        print(
            f"{'frame':>7} {'prev':>7} {'time_s':>9} "
            f"{'dx_px':>10} {'dy_px':>10} {'dist_px':>10} "
            f"{'cum_x_px':>11} {'cum_y_px':>11} {'resp':>7}"
        )
        for r in records:
            print(
                f"{r.frame:7d} {r.prev_frame:7d} {r.time_s:9.3f} "
                f"{r.dx:+10.3f} {r.dy:+10.3f} {r.dist:10.3f} "
                f"{r.cum_x:+11.2f} {r.cum_y:+11.2f} {r.response:7.3f}"
            )

    csv_out: Path | None = None
    if write_csv:
        csv_out = args.csv if args.csv is not None else base_dir / f"{base_stem}_offsets.csv"
        write_csv_report(csv_out, records)

    png_out: Path | None = None
    if not args.no_plot:
        png_out = args.plot if args.plot is not None else base_dir / f"{base_stem}_trajectory.png"
        write_trajectory_png(png_out, records, title_info)

    svg_out: Path | None = None
    if not args.no_svg:
        svg_out = args.svg if args.svg is not None else base_dir / f"{base_stem}_trajectory.svg"
        write_trajectory_svg(svg_out, records, title_info)

    elapsed = time.perf_counter() - t0
    steps = [r.dist for r in records[1:]] if len(records) > 1 else [0.0]
    path_len = float(sum(steps))
    mean_step = float(np.mean(steps))
    med_step = float(np.median(steps))
    max_r = max(records[1:], key=lambda r: r.dist) if len(records) > 1 else records[0]

    print(
        f"Scanned {len(records)} frames in {elapsed:.2f}s | "
        f"net offset=({records[-1].cum_x:+.2f}, {records[-1].cum_y:+.2f}) px | "
        f"path={path_len:.2f} px | "
        f"step mean={mean_step:.3f}, median={med_step:.3f}, "
        f"max={max_r.dist:.3f} px at frame {max_r.frame} ({max_r.dx:+.3f}, {max_r.dy:+.3f})"
    )
    if csv_out is not None:
        print(f"CSV report : {csv_out}")
    if png_out is not None:
        print(f"PNG plot   : {png_out}")
    if svg_out is not None:
        print(f"SVG vector : {svg_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
