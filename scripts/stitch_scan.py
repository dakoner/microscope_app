#!/usr/bin/env python3
"""Offline optical subpixel stitcher for microscope_app scan directories.

Reads ``scan_capture.mkv`` and ``scan_metadata.json`` from a scan directory,
uses the recorded interpolated CNC stage poses as initial guesses, refines
intra-strip frame-to-frame motion and inter-strip column overlaps via subpixel
normalized cross-correlation, applies smooth flat-field vignette correction,
and blends frames with center-weighted cosine feathering into a lossless TIFF.

Uses a streaming multi-pass design so memory stays bounded regardless of how
many frames the scan MKV contains.

Usage:
    uv run scripts/stitch_scan.py videos/scan_region_01_1795480133
"""

from __future__ import annotations

import argparse
import gc
import json
import time
from pathlib import Path

import cv2
import numpy as np


def _rotate_frame(bgr: np.ndarray, rot_deg: int) -> np.ndarray:
    if rot_deg == 90:
        return cv2.rotate(bgr, cv2.ROTATE_90_CLOCKWISE)
    if rot_deg == 180:
        return cv2.rotate(bgr, cv2.ROTATE_180)
    if rot_deg == 270:
        return cv2.rotate(bgr, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return bgr


def _subpixel_peak(res: np.ndarray, mx: int, my: int) -> tuple[float, float]:
    sx, sy = float(mx), float(my)
    if 1 <= mx < res.shape[1] - 1:
        vl, vc, vr = float(res[my, mx - 1]), float(res[my, mx]), float(res[my, mx + 1])
        denom = vl - 2.0 * vc + vr
        if abs(denom) > 1e-9:
            delta = 0.5 * (vl - vr) / denom
            if abs(delta) <= 1.0:
                sx += delta
    if 1 <= my < res.shape[0] - 1:
        vu, vc, vd = float(res[my - 1, mx]), float(res[my, mx]), float(res[my + 1, mx])
        denom = vu - 2.0 * vc + vd
        if abs(denom) > 1e-9:
            delta = 0.5 * (vu - vd) / denom
            if abs(delta) <= 1.0:
                sy += delta
    return sx, sy


def _highpass_channel(img_bgr: np.ndarray, bg_gray: np.ndarray, chan: int) -> np.ndarray:
    ch = img_bgr[:, :, chan].astype(np.float32) - bg_gray
    # sigma=25 is purely low-frequency; blurring at 1/4 resolution is ~10x faster and visually identical.
    h, w = ch.shape
    small = cv2.resize(ch, (max(1, w // 4), max(1, h // 4)), interpolation=cv2.INTER_AREA)
    low_small = cv2.GaussianBlur(small, (0, 0), 25.0 / 4.0)
    low = cv2.resize(low_small, (w, h), interpolation=cv2.INTER_LINEAR)
    return ch - low


def _match_pair_subpixel(
    hp_a: np.ndarray,
    hp_b: np.ndarray,
    pred_fdx: float,
    pred_fdy: float,
    search_rx: int = 24,
    search_ry: int = 75,
    ds: int = 2,
) -> tuple[float, float, float, float] | None:
    """Measure feature shift (fdx, fdy) where feature at (u, v) in A moves to (u+fdx, v+fdy) in B.

    Returns (fdx, fdy, corr, patch_std) or None if overlap is too small.
    """
    H, W = hp_a.shape
    pdx_i = int(round(pred_fdx))
    pdy_i = int(round(pred_fdy))

    px0 = max(32, -pdx_i + search_rx + 8)
    px1 = min(W - 32, W - pdx_i - search_rx - 8)
    py0 = max(32, -pdy_i + search_ry + 8)
    py1 = min(H - 32, H - pdy_i - search_ry - 8)
    if px1 - px0 < 160 or py1 - py0 < 160:
        return None

    patch_full = hp_a[py0:py1, px0:px1]
    patch_std = float(patch_full.std())
    if patch_std < 2.2:
        return 0.0, 0.0, 0.0, patch_std

    sx0 = px0 + pdx_i - search_rx
    sy0 = py0 + pdy_i - search_ry
    sx1 = px1 + pdx_i + search_rx
    sy1 = py1 + pdy_i + search_ry
    search_full = hp_b[sy0:sy1, sx0:sx1]

    if ds > 1:
        p_s = cv2.resize(patch_full, (patch_full.shape[1] // ds, patch_full.shape[0] // ds), interpolation=cv2.INTER_AREA)
        s_s = cv2.resize(search_full, (search_full.shape[1] // ds, search_full.shape[0] // ds), interpolation=cv2.INTER_AREA)
        if s_s.shape[0] <= p_s.shape[0] or s_s.shape[1] <= p_s.shape[1]:
            return None
        res_c = cv2.matchTemplate(s_s, p_s, cv2.TM_CCOEFF_NORMED)
        _, _, _, ml_c = cv2.minMaxLoc(res_c)
        cdx = (sx0 + ml_c[0] * ds) - px0
        cdy = (sy0 + ml_c[1] * ds) - py0
    else:
        cdx, cdy = pdx_i, pdy_i

    rad = 6
    rx0 = max(0, px0 + cdx - rad)
    ry0 = max(0, py0 + cdy - rad)
    rx1 = min(W, px1 + cdx + rad)
    ry1 = min(H, py1 + cdy + rad)
    pw = px1 - px0
    ph = py1 - py0
    if rx1 - rx0 <= pw or ry1 - ry0 <= ph:
        return None

    res_f = cv2.matchTemplate(hp_b[ry0:ry1, rx0:rx1], patch_full, cv2.TM_CCOEFF_NORMED)
    _, max_val, _, max_loc = cv2.minMaxLoc(res_f)
    sub_x, sub_y = _subpixel_peak(res_f, max_loc[0], max_loc[1])
    fdx = (rx0 + sub_x) - px0
    fdy = (ry0 + sub_y) - py0
    return fdx, fdy, float(max_val), patch_std


def _solve_strip_positions(
    x0: np.ndarray,
    y0: np.ndarray,
    opt_edges: list[tuple[int, int, float, float, float]],
    pose_weight: float = 1.0,
) -> tuple[np.ndarray, np.ndarray]:
    """Solve 1D weighted pose graph per axis for a single strip."""
    n = len(x0)
    if n <= 1:
        return x0.copy(), y0.copy()

    Ax = np.zeros((n, n), dtype=np.float64)
    bx = np.zeros(n, dtype=np.float64)
    Ay = np.zeros((n, n), dtype=np.float64)
    by = np.zeros(n, dtype=np.float64)

    for k in range(n - 1):
        dx_p = x0[k + 1] - x0[k]
        dy_p = y0[k + 1] - y0[k]
        w = pose_weight
        Ax[k, k] += w
        Ax[k + 1, k + 1] += w
        Ax[k, k + 1] -= w
        Ax[k + 1, k] -= w
        bx[k] -= w * dx_p
        bx[k + 1] += w * dx_p

        Ay[k, k] += w
        Ay[k + 1, k + 1] += w
        Ay[k, k + 1] -= w
        Ay[k + 1, k] -= w
        by[k] -= w * dy_p
        by[k + 1] += w * dy_p

    for i, j, dx_m, dy_m, w in opt_edges:
        Ax[i, i] += w
        Ax[j, j] += w
        Ax[i, j] -= w
        Ax[j, i] -= w
        bx[i] -= w * dx_m
        bx[j] += w * dx_m

        Ay[i, i] += w
        Ay[j, j] += w
        Ay[i, j] -= w
        Ay[j, i] -= w
        by[i] -= w * dy_m
        by[j] += w * dy_m

    w_anc = 1e-2 / n
    Ax += w_anc
    bx += w_anc * np.sum(x0)
    Ay += w_anc
    by += w_anc * np.sum(y0)

    return np.linalg.solve(Ax, bx), np.linalg.solve(Ay, by)


def _make_feather_weight(h: int, w: int) -> np.ndarray:
    wy = np.sin(np.pi * (np.arange(h, dtype=np.float32) + 0.5) / h) ** 2
    wx = np.sin(np.pi * (np.arange(w, dtype=np.float32) + 0.5) / w) ** 2
    mask = np.outer(wy, wx)
    return np.maximum(mask, 1e-4).astype(np.float32)


def _iter_selected_frames(mkv_path: Path, selected: set[int], max_idx: int, rot_deg: int):
    """Stream MKV sequentially, decoding only frame indices in `selected`."""
    cap = cv2.VideoCapture(str(mkv_path))
    if not cap.isOpened():
        raise RuntimeError(f"Could not open video: {mkv_path}")
    try:
        idx = 0
        while idx <= max_idx:
            if idx in selected:
                ok, bgr = cap.read()
                if not ok:
                    break
                yield idx, _rotate_frame(bgr, rot_deg)
            else:
                if not cap.grab():
                    break
            idx += 1
    finally:
        cap.release()


def stitch_scan(
    scan_dir: Path,
    output_path: Path | None = None,
    *,
    flatfield: bool = True,
    frame_stride: int = 1,
    min_corr: float = 0.72,
    save_debug_dir: Path | None = None,
) -> Path:
    t0 = time.perf_counter()
    meta_path = scan_dir / "scan_metadata.json"
    mkv_path = scan_dir / "scan_capture.mkv"
    if not meta_path.is_file() or not mkv_path.is_file():
        raise FileNotFoundError(f"Expected scan_metadata.json and scan_capture.mkv in {scan_dir}")

    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    frames_meta = meta["frames"]
    n_meta = len(frames_meta)
    if n_meta == 0:
        raise RuntimeError("No frames recorded in scan_metadata.json.")

    scale_x = float(meta.get("scan_scale_x_px_per_mm") or meta["pixel_scale_px_per_mm"])
    scale_y = float(meta.get("scan_scale_y_px_per_mm") or meta["pixel_scale_px_per_mm"])
    rot_deg = int(meta.get("scan_camera_rotation_deg", 0))
    x_min = float(meta["scan_area_mm"]["x_min"])
    y_min = float(meta["scan_area_mm"]["y_min"])
    x_max = float(meta["scan_area_mm"]["x_max"])
    y_max = float(meta["scan_area_mm"]["y_max"])

    print(f"Scanning MKV (streaming): {mkv_path} ({n_meta} metadata frames, scale={scale_x:.2f}x{scale_y:.2f} px/mm)")

    # --- Pass 0: Sample <= 64 frames to measure background, best tracking channel, and flat-field ---
    sample_step = max(1, n_meta // 64)
    sample_indices = set(range(0, n_meta, sample_step))
    samples: list[np.ndarray] = []
    for _, bgr in _iter_selected_frames(mkv_path, sample_indices, n_meta - 1, rot_deg):
        samples.append(bgr)
    if not samples:
        raise RuntimeError("No frames decoded from MKV.")

    H, W = samples[0].shape[:2]
    sample_stack = np.stack(samples, axis=0).astype(np.float32)
    del samples
    chan_means = sample_stack.mean(axis=(0, 1, 2), dtype=np.float64)
    best_chan = int(np.argmax(chan_means))
    bg_bgr = np.median(sample_stack, axis=0)
    del sample_stack
    gc.collect()

    bg_chan = bg_bgr[:, :, best_chan].copy()
    print(f"Frame size {W}x{H}, mean BGR={chan_means.round(1).tolist()} -> tracking on channel {best_chan}")

    if flatfield:
        vignette = cv2.GaussianBlur(bg_bgr, (0, 0), 80.0)
        center_val = vignette[H // 2 - 64:H // 2 + 64, W // 2 - 64:W // 2 + 64].mean(axis=(0, 1))
        gain_map: np.ndarray | None = np.ones_like(vignette, dtype=np.float32)
        for c in range(3):
            if center_val[c] > 20.0:
                gain_map[:, :, c] = np.clip(center_val[c] / np.maximum(vignette[:, :, c], 1.0), 0.7, 1.4)
        del vignette
    else:
        gain_map = None
    del bg_bgr

    fov_x_mm = W / scale_x
    fov_y_mm = H / scale_y

    # --- Pass 1: Stream MKV sequentially (only 2 frames in RAM!) for intra-strip registration ---
    print("\n--- Step 1: Streaming intra-strip optical registration ---")
    init_x = np.array([(fr["stage_x_mm"] - fov_x_mm / 2.0 - x_min) * scale_x for fr in frames_meta], dtype=np.float64)
    init_y = np.array([(fr["stage_y_mm"] - fov_y_mm / 2.0 - y_min) * scale_y for fr in frames_meta], dtype=np.float64)
    seg_arr = np.array([int(fr["segment_number"]) for fr in frames_meta], dtype=np.int32)

    # Estimate nominal right-edge overlap band width for quick texture scoring during Pass 1
    segs_meta: dict[int, list[int]] = {}
    for i, s in enumerate(seg_arr):
        segs_meta.setdefault(int(s), []).append(i)
    seg_ids_all = sorted(segs_meta.keys())
    if len(seg_ids_all) >= 2:
        nom_step_px = float(np.median([np.median(init_x[segs_meta[seg_ids_all[i + 1]]]) - np.median(init_x[segs_meta[seg_ids_all[i]]])
                                       for i in range(len(seg_ids_all) - 1)]))
        nom_ov_w = int(round(W - nom_step_px))
    else:
        nom_ov_w = W // 4
    r_px0 = max(0, min(W - 64, max(W - max(80, nom_ov_w) + 24, W - 260)))
    r_px1 = W - 20

    right_band_stds = np.zeros(n_meta, dtype=np.float32)
    edges_by_seg: dict[int, list[tuple[int, int, float, float, float]]] = {sid: [] for sid in seg_ids_all}
    local_k_by_idx = np.zeros(n_meta, dtype=np.int32)
    for sid, idxs in segs_meta.items():
        for k, idx in enumerate(idxs):
            local_k_by_idx[idx] = k

    intra_residuals_y: list[float] = []
    cap = cv2.VideoCapture(str(mkv_path))
    if not cap.isOpened():
        raise RuntimeError(f"Could not open video: {mkv_path}")
    prev_hp: np.ndarray | None = None
    prev_idx = -1
    prev_seg = -1
    n_decoded = 0
    try:
        for i in range(n_meta):
            ok, bgr = cap.read()
            if not ok:
                break
            n_decoded = i + 1
            bgr = _rotate_frame(bgr, rot_deg)
            cur_hp = _highpass_channel(bgr, bg_chan, best_chan)
            del bgr
            right_band_stds[i] = float(cur_hp[180:H - 180, r_px0:r_px1].std())

            cur_seg = int(seg_arr[i])
            if prev_hp is not None and cur_seg == prev_seg:
                pred_fdx = -(init_x[i] - init_x[prev_idx])
                pred_fdy = -(init_y[i] - init_y[prev_idx])
                m = _match_pair_subpixel(prev_hp, cur_hp, pred_fdx, pred_fdy, search_rx=24, search_ry=75, ds=2)
                if m is not None:
                    fdx, fdy, corr, pstd = m
                    if corr >= min_corr and pstd >= 2.8:
                        meas_dx = -fdx
                        meas_dy = -fdy
                        if abs(meas_dx - (init_x[i] - init_x[prev_idx])) < 22.0 and abs(meas_dy - (init_y[i] - init_y[prev_idx])) < 65.0:
                            k_prev = int(local_k_by_idx[prev_idx])
                            edges_by_seg[cur_seg].append((k_prev, k_prev + 1, meas_dx, meas_dy, 150.0 * (corr ** 2)))
                            intra_residuals_y.append(meas_dy - (init_y[i] - init_y[prev_idx]))

            prev_hp = cur_hp
            prev_idx = i
            prev_seg = cur_seg
    finally:
        cap.release()
    prev_hp = None

    if n_decoded == 0:
        raise RuntimeError("No frames decoded from MKV in Pass 1.")
    if n_decoded != n_meta:
        print(f"Warning: MKV frame count ({n_decoded}) != metadata count ({n_meta}), using {n_decoded}.")
    n_frames = n_decoded

    segs: dict[int, list[int]] = {}
    for i in range(n_frames):
        segs.setdefault(int(seg_arr[i]), []).append(i)
    seg_ids = sorted(segs.keys())

    pos_x = init_x[:n_frames].copy()
    pos_y = init_y[:n_frames].copy()
    strip_textured_counts: dict[int, int] = {}

    for sid in seg_ids:
        idxs = segs[sid]
        x0 = init_x[idxs]
        y0 = init_y[idxs]
        opt_edges = [e for e in edges_by_seg[sid] if e[1] < len(idxs)]
        xs, ys = _solve_strip_positions(x0, y0, opt_edges, pose_weight=1.0)
        pos_x[idxs] = xs
        pos_y[idxs] = ys
        strip_textured_counts[sid] = len(opt_edges)
        print(f"  Strip {sid}: {len(idxs):3d} frames, {len(opt_edges):3d} optical edges, "
              f"max adjustment vs stage pose: dX={float(np.max(np.abs(xs - x0))):.1f} px, "
              f"dY={float(np.max(np.abs(ys - y0))):.1f} px")

    if intra_residuals_y:
        arr = np.array(intra_residuals_y)
        print(f"  Step-to-step optical vs stage pose Y diff: rms={np.sqrt(np.mean(arr**2)):.2f} px, p95={np.percentile(np.abs(arr),95):.2f} px")

    # --- Pass 2: Selective decode ONLY for inter-strip seam candidate frames ---
    print("\n--- Step 2: Inter-strip overlap alignment ---")
    n_strips = len(seg_ids)
    seam_plans: list[tuple[int, int, int, int, list[tuple[int, list[int]]]]] = []
    needed_right: set[int] = set()
    needed_left: set[int] = set()
    left_crop_w = min(W, max(400, nom_ov_w + 260))

    for i in range(n_strips - 1):
        s1, s2 = seg_ids[i], seg_ids[i + 1]
        id1, id2 = segs[s1], segs[s2]
        nom_dx = float(np.median(pos_x[id2]) - np.median(pos_x[id1]))
        ov_w = int(round(W - nom_dx))
        if ov_w < 80 or ov_w >= W - 64:
            print(f"  Strip {s1}->{s2}: unexpected overlap {ov_w} px, skipping")
            continue

        px0 = max(W - ov_w + 24, W - 260)
        px1 = W - 20
        r_stds = right_band_stds[id1]
        cands = np.where(r_stds > 4.0)[0]
        if len(cands) == 0:
            print(f"  Strip {s1}->{s2}: blank overlap (max std={r_stds.max():.1f}), keeping stage pose")
            continue

        picks = cands[np.linspace(0, len(cands) - 1, min(16, len(cands))).astype(int)]
        y2_arr = pos_y[id2]
        pair_list: list[tuple[int, list[int]]] = []
        for k1 in picks:
            f1 = id1[int(k1)]
            k2_c = int(np.argmin(np.abs(y2_arr - pos_y[f1])))
            f2s = [id2[k2] for k2 in range(max(0, k2_c - 5), min(len(id2), k2_c + 6))]
            pair_list.append((f1, f2s))
            needed_right.add(f1)
            needed_left.update(f2s)
        seam_plans.append((i, s1, s2, px0, px1, pair_list))  # type: ignore[arg-type]

    needed_all = needed_right | needed_left
    cache_r_px0 = min([r_px0] + [p[3] for p in seam_plans]) if seam_plans else r_px0
    right_cache: dict[int, np.ndarray] = {}
    left_cache: dict[int, np.ndarray] = {}
    if needed_all:
        print(f"  Caching overlap bands for {len(needed_all)}/{n_frames} frames...")
        for f_idx, bgr in _iter_selected_frames(mkv_path, needed_all, max(needed_all), rot_deg):
            hp = _highpass_channel(bgr, bg_chan, best_chan)
            if f_idx in needed_right:
                right_cache[f_idx] = hp[:, cache_r_px0:W].astype(np.float16)
            if f_idx in needed_left:
                left_cache[f_idx] = hp[:, 0:left_crop_w].astype(np.float16)
            del hp, bgr

    seam_edges: list[tuple[int, int, float, float, float]] = []
    for i, s1, s2, px0, px1, pair_list in seam_plans:  # type: ignore[misc]
        obs_dx_list: list[float] = []
        obs_dy_list: list[float] = []
        obs_corr_list: list[float] = []
        pw = px1 - px0
        ph = (H - 180) - 180

        for f1, f2s in pair_list:
            hp1_r = right_cache.get(f1)
            if hp1_r is None:
                continue
            off1 = px0 - cache_r_px0
            if off1 < 0 or off1 + pw > hp1_r.shape[1]:
                continue
            patch = hp1_r[180:H - 180, off1:off1 + pw].astype(np.float32)
            x1_cur = pos_x[f1]
            y1_cur = pos_y[f1]
            best = None

            for f2 in f2s:
                hp2_l = left_cache.get(f2)
                if hp2_l is None:
                    continue
                pred_fdx = x1_cur - pos_x[f2]
                pred_fdy = y1_cur - pos_y[f2]
                ex0 = int(round(px0 + pred_fdx))
                ey0 = int(round(180 + pred_fdy))
                sx0 = max(0, ex0 - 80)
                sy0 = max(0, ey0 - 90)
                sx1 = min(hp2_l.shape[1], ex0 + pw + 80)
                sy1 = min(H, ey0 + ph + 90)
                if sx1 - sx0 <= pw + 4 or sy1 - sy0 <= ph + 4:
                    continue
                res = cv2.matchTemplate(hp2_l[sy0:sy1, sx0:sx1].astype(np.float32), patch, cv2.TM_CCOEFF_NORMED)
                _, mv, _, ml = cv2.minMaxLoc(res)
                if best is None or mv > best[0]:
                    sub_x, sub_y = _subpixel_peak(res, ml[0], ml[1])
                    fdx = (sx0 + sub_x) - px0
                    fdy = (sy0 + sub_y) - 180.0
                    dX_req = (x1_cur - pos_x[f2]) - fdx
                    dY_req = (y1_cur - pos_y[f2]) - fdy
                    best = (float(mv), dX_req, dY_req)

            if best is not None and best[0] >= min_corr:
                obs_corr_list.append(best[0])
                obs_dx_list.append(best[1])
                obs_dy_list.append(best[2])

        if len(obs_dx_list) >= 2:
            dxs = np.array(obs_dx_list)
            dys = np.array(obs_dy_list)
            med_dx = float(np.median(dxs))
            med_dy = float(np.median(dys))
            inliers = (np.abs(dxs - med_dx) < 8.0) & (np.abs(dys - med_dy) < 12.0)
            if np.sum(inliers) >= 2:
                m_dx = float(np.median(dxs[inliers]))
                m_dy = float(np.median(dys[inliers]))
                std_dx = float(np.std(dxs[inliers]))
                std_dy = float(np.std(dys[inliers]))
                seam_edges.append((i, i + 1, m_dx, m_dy, 100.0 * float(np.sum(inliers))))
                print(f"  Strip {s1}->{s2}: {int(np.sum(inliers))}/{len(dxs)} inliers "
                      f"(med_corr={np.median(obs_corr_list):.4f}), "
                      f"seam shift=(dX={m_dx:+.2f}±{std_dx:.2f}, dY={m_dy:+.2f}±{std_dy:.2f}) px")
            else:
                print(f"  Strip {s1}->{s2}: inconsistent matches, keeping stage pose")
        else:
            print(f"  Strip {s1}->{s2}: only {len(obs_dx_list)} matches >= {min_corr}, keeping stage pose")

    right_cache.clear()
    left_cache.clear()
    gc.collect()

    anchor_strip = max(range(n_strips), key=lambda i: strip_textured_counts[seg_ids[i]])
    Sx = np.zeros((n_strips, n_strips), dtype=np.float64)
    bx_s = np.zeros(n_strips, dtype=np.float64)
    Sy = np.zeros((n_strips, n_strips), dtype=np.float64)
    by_s = np.zeros(n_strips, dtype=np.float64)

    for i in range(n_strips - 1):
        w = 1.0
        for M in (Sx, Sy):
            M[i, i] += w
            M[i + 1, i + 1] += w
            M[i, i + 1] -= w
            M[i + 1, i] -= w

    for i, j, dx_req, dy_req, w in seam_edges:
        Sx[i, i] += w
        Sx[j, j] += w
        Sx[i, j] -= w
        Sx[j, i] -= w
        bx_s[i] -= w * dx_req
        bx_s[j] += w * dx_req

        Sy[i, i] += w
        Sy[j, j] += w
        Sy[i, j] -= w
        Sy[j, i] -= w
        by_s[i] -= w * dy_req
        by_s[j] += w * dy_req

    Sx[anchor_strip, anchor_strip] += 1e4
    Sy[anchor_strip, anchor_strip] += 1e4

    delta_sx = np.linalg.solve(Sx, bx_s)
    delta_sy = np.linalg.solve(Sy, by_s)
    print("  Solved strip offsets (px):",
          ", ".join(f"S{seg_ids[i]}=({delta_sx[i]:+.1f},{delta_sy[i]:+.1f})" for i in range(n_strips)))

    for i, sid in enumerate(seg_ids):
        idxs = segs[sid]
        pos_x[idxs] += delta_sx[i]
        pos_y[idxs] += delta_sy[i]

    # --- Pass 3: Streaming subpixel center-weighted feather blending ---
    print("\n--- Step 3: Streaming subpixel feather blending ---")
    out_w = max(1, int(np.ceil((x_max - x_min) * scale_x)))
    out_h = max(1, int(np.ceil((y_max - y_min) * scale_y)))
    base_weight = _make_feather_weight(H, W)

    chosen_set: set[int] = set()
    for sid in seg_ids:
        idxs = segs[sid]
        picked = idxs[::max(1, frame_stride)]
        if idxs[-1] != picked[-1]:
            picked = picked + [idxs[-1]]
        chosen_set.update(picked)

    # Tile horizontally in Y-bands when canvas is very large so peak RAM stays <= ~1.8 GB
    max_band_pixels = 100_000_000  # ~1.6 GB for float32 BGR + float32 W
    if out_w * out_h <= max_band_pixels:
        band_height = out_h
    else:
        band_height = max(H, max_band_pixels // out_w)

    out_img = np.full((out_h, out_w, 3), 255, dtype=np.uint8)
    blended_total = 0
    valid_pixels = 0

    num_bands = (out_h + band_height - 1) // band_height
    if num_bands > 1:
        print(f"  Large canvas ({out_w}x{out_h}): blending in {num_bands} Y-bands of {band_height} px to bound RAM.")

    for band_idx in range(num_bands):
        by0 = band_idx * band_height
        by1 = min(out_h, by0 + band_height)
        bh = by1 - by0

        band_frames = {
            f_idx for f_idx in chosen_set
            if pos_y[f_idx] + H + 1 > by0 and pos_y[f_idx] < by1
        }
        if not band_frames:
            continue

        accum_bgr = np.zeros((bh, out_w, 3), dtype=np.float32)
        accum_w = np.zeros((bh, out_w), dtype=np.float32)

        for f_idx, bgr in _iter_selected_frames(mkv_path, band_frames, max(band_frames), rot_deg):
            x_f = float(pos_x[f_idx])
            y_f = float(pos_y[f_idx])
            ix = int(np.floor(x_f))
            iy = int(np.floor(y_f))
            fx = x_f - ix
            fy = y_f - iy

            tw, th = W + 1, H + 1
            cx0 = max(0, ix)
            cy0 = max(by0, iy)
            cx1 = min(out_w, ix + tw)
            cy1 = min(by1, iy + th)
            if cx1 <= cx0 or cy1 <= cy0:
                continue

            img_f = bgr.astype(np.float32)
            if gain_map is not None:
                img_f *= gain_map

            M = np.array([[1.0, 0.0, fx], [0.0, 1.0, fy]], dtype=np.float32)
            shifted_bgr = cv2.warpAffine(img_f, M, (tw, th), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0))
            shifted_w = cv2.warpAffine(base_weight, M, (tw, th), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0.0)

            sx0 = cx0 - ix
            sy0 = cy0 - iy
            sx1 = cx1 - ix
            sy1 = cy1 - iy

            w_crop = shifted_w[sy0:sy1, sx0:sx1]
            accum_bgr[cy0 - by0:cy1 - by0, cx0:cx1] += shifted_bgr[sy0:sy1, sx0:sx1] * w_crop[:, :, None]
            accum_w[cy0 - by0:cy1 - by0, cx0:cx1] += w_crop
            blended_total += 1

        valid = accum_w > 1e-6
        valid_pixels += int(np.sum(valid))
        band_out = out_img[by0:by1]
        band_out[valid] = np.clip(accum_bgr[valid] / accum_w[valid, None], 0.0, 255.0).astype(np.uint8)
        del accum_bgr, accum_w, valid
        gc.collect()

    if output_path is None:
        output_path = scan_dir / "scan_composite_stitched.tif"

    ok = cv2.imwrite(str(output_path), out_img, [cv2.IMWRITE_TIFF_COMPRESSION, 5])
    if not ok:
        ok = cv2.imwrite(str(output_path), out_img)
    if not ok:
        raise RuntimeError(f"Failed to write {output_path}")

    elapsed = time.perf_counter() - t0
    coverage = 100.0 * valid_pixels / float(out_w * out_h)
    print(f"Saved {output_path} ({out_w}x{out_h}, {len(chosen_set)} frames selected / {blended_total} tile writes, "
          f"coverage={coverage:.2f}%, {output_path.stat().st_size/1e6:.2f} MB) in {elapsed:.1f}s")

    if save_debug_dir is not None:
        save_debug_dir.mkdir(parents=True, exist_ok=True)
        cv2.imwrite(str(save_debug_dir / f"{scan_dir.name}_stitched_overview.png"),
                    cv2.resize(out_img, (max(1, out_w // 8), max(1, out_h // 8)), interpolation=cv2.INTER_AREA))

    return output_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Streaming optical subpixel stitcher for microscope scan directories.")
    parser.add_argument("scan_dir", type=Path, help="Path to scan directory containing scan_capture.mkv and scan_metadata.json.")
    parser.add_argument("-o", "--output", type=Path, default=None, help="Output TIFF path (default: <scan_dir>/scan_composite_stitched.tif).")
    parser.add_argument("--no-flatfield", action="store_true", help="Disable smooth illumination flat-field normalization.")
    parser.add_argument("--stride", type=int, default=1, help="Blend every Nth frame per strip (default: 1 = all frames).")
    parser.add_argument("--min-corr", type=float, default=0.72, help="Minimum NCC correlation threshold (default: 0.72).")
    parser.add_argument("--debug-dir", type=Path, default=None, help="Optional directory to save overview verification PNG.")
    args = parser.parse_args()

    stitch_scan(
        args.scan_dir,
        args.output,
        flatfield=not args.no_flatfield,
        frame_stride=args.stride,
        min_corr=args.min_corr,
        save_debug_dir=args.debug_dir,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
