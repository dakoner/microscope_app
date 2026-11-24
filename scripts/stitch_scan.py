#!/usr/bin/env python3
"""Offline optical subpixel stitcher for microscope_app scan directories.

Reads ``scan_capture.mkv`` and ``scan_metadata.json`` from a scan directory,
uses the recorded interpolated CNC stage poses as initial guesses, refines
intra-strip frame-to-frame motion and inter-strip column overlaps via subpixel
normalized cross-correlation, applies smooth flat-field vignette correction,
and blends frames with center-weighted cosine feathering into a lossless TIFF.

Usage:
    uv run scripts/stitch_scan.py videos/scan_region_01_1795480133
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import cv2
import numpy as np


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
    return ch - cv2.GaussianBlur(ch, (0, 0), 25.0)


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
    # Choose a generous patch inside A that fits inside B after shifting by pred + search radius.
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

    # Coarse search at 1/ds resolution
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

    # Full-resolution refinement +-6 px around coarse shift
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
    """Solve 1D weighted pose graph per axis for a single strip.

    opt_edges: list of (k, k+1, measured_dX, measured_dY, weight) where dX = X_{k+1} - X_k.
    """
    n = len(x0)
    if n <= 1:
        return x0.copy(), y0.copy()

    Ax = np.zeros((n, n), dtype=np.float64)
    bx = np.zeros(n, dtype=np.float64)
    Ay = np.zeros((n, n), dtype=np.float64)
    by = np.zeros(n, dtype=np.float64)

    # Weak relative stage-pose prior on every consecutive step (bridges blank glass smoothly)
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

    # Strong optical edges where features correlate cleanly
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

    # Centroid anchor keeps strip aligned to stage coordinate origin without fighting rotation/scale
    w_anc = 1e-2 / n
    Ax += w_anc
    bx += w_anc * np.sum(x0)
    Ay += w_anc
    by += w_anc * np.sum(y0)

    x_sol = np.linalg.solve(Ax, bx)
    y_sol = np.linalg.solve(Ay, by)
    return x_sol, y_sol


def _make_feather_weight(h: int, w: int) -> np.ndarray:
    wy = np.sin(np.pi * (np.arange(h, dtype=np.float32) + 0.5) / h) ** 2
    wx = np.sin(np.pi * (np.arange(w, dtype=np.float32) + 0.5) / w) ** 2
    mask = np.outer(wy, wx)
    return np.maximum(mask, 1e-4).astype(np.float32)


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
    scale_x = float(meta.get("scan_scale_x_px_per_mm") or meta["pixel_scale_px_per_mm"])
    scale_y = float(meta.get("scan_scale_y_px_per_mm") or meta["pixel_scale_px_per_mm"])
    rot_deg = int(meta.get("scan_camera_rotation_deg", 0))
    x_min = float(meta["scan_area_mm"]["x_min"])
    y_min = float(meta["scan_area_mm"]["y_min"])
    x_max = float(meta["scan_area_mm"]["x_max"])
    y_max = float(meta["scan_area_mm"]["y_max"])

    print(f"Loading MKV: {mkv_path} ({len(frames_meta)} metadata frames, scale={scale_x:.2f}x{scale_y:.2f} px/mm)")
    cap = cv2.VideoCapture(str(mkv_path))
    frames_bgr: list[np.ndarray] = []
    while True:
        ok, bgr = cap.read()
        if not ok:
            break
        if rot_deg == 90:
            bgr = cv2.rotate(bgr, cv2.ROTATE_90_CLOCKWISE)
        elif rot_deg == 180:
            bgr = cv2.rotate(bgr, cv2.ROTATE_180)
        elif rot_deg == 270:
            bgr = cv2.rotate(bgr, cv2.ROTATE_90_COUNTERCLOCKWISE)
        frames_bgr.append(bgr)
    cap.release()

    n_frames = min(len(frames_bgr), len(frames_meta))
    if n_frames == 0:
        raise RuntimeError("No frames decoded from MKV.")
    if len(frames_bgr) != len(frames_meta):
        print(f"Warning: MKV frame count ({len(frames_bgr)}) != metadata count ({len(frames_meta)}), using {n_frames}.")
    frames_bgr = frames_bgr[:n_frames]
    frames_meta = frames_meta[:n_frames]

    H, W = frames_bgr[0].shape[:2]
    fov_x_mm = W / scale_x
    fov_y_mm = H / scale_y

    # Choose highest-signal BGR channel for correlation (e.g., Red under red LED illumination)
    chan_means = np.mean(np.stack(frames_bgr[::max(1, n_frames // 40)], axis=0), axis=(0, 1, 2))
    best_chan = int(np.argmax(chan_means))
    print(f"Decoded {n_frames} frames ({W}x{H}), mean BGR={chan_means.round(1).tolist()} -> tracking on channel {best_chan}")

    # Estimate fixed-pattern background & smooth illumination flat-field
    sample_stack = np.stack(frames_bgr[::max(1, n_frames // 80)], axis=0).astype(np.float32)
    bg_bgr = np.median(sample_stack, axis=0)
    bg_chan = bg_bgr[:, :, best_chan]

    if flatfield:
        # Smooth illumination vignette map (sigma=80 removes dust/features, preserves lens shading)
        vignette = cv2.GaussianBlur(bg_bgr, (0, 0), 80.0)
        center_val = vignette[H // 2 - 64:H // 2 + 64, W // 2 - 64:W // 2 + 64].mean(axis=(0, 1))
        # Avoid amplifying near-zero dark channels (e.g., B/G under red LED)
        gain_map = np.ones_like(vignette, dtype=np.float32)
        for c in range(3):
            if center_val[c] > 20.0:
                gain_map[:, :, c] = np.clip(center_val[c] / np.maximum(vignette[:, :, c], 1.0), 0.7, 1.4)
    else:
        gain_map = None

    # Precompute high-pass tracking frames
    hps = [_highpass_channel(im, bg_chan, best_chan) for im in frames_bgr]

    # Group frame indices by segment
    segs: dict[int, list[int]] = {}
    for i, fr in enumerate(frames_meta):
        segs.setdefault(int(fr["segment_number"]), []).append(i)
    seg_ids = sorted(segs.keys())

    # Initial canvas positions from interpolated stage poses
    init_x = np.array([(fr["stage_x_mm"] - fov_x_mm / 2.0 - x_min) * scale_x for fr in frames_meta], dtype=np.float64)
    init_y = np.array([(fr["stage_y_mm"] - fov_y_mm / 2.0 - y_min) * scale_y for fr in frames_meta], dtype=np.float64)

    pos_x = init_x.copy()
    pos_y = init_y.copy()

    # Step 1: Intra-strip optical subpixel registration
    print("\n--- Step 1: Intra-strip optical registration ---")
    strip_textured_counts: dict[int, int] = {}
    intra_residuals_y: list[float] = []
    for sid in seg_ids:
        idxs = segs[sid]
        x0 = init_x[idxs]
        y0 = init_y[idxs]
        opt_edges: list[tuple[int, int, float, float, float]] = []
        for k in range(len(idxs) - 1):
            pred_fdx = -(x0[k + 1] - x0[k])
            pred_fdy = -(y0[k + 1] - y0[k])
            m = _match_pair_subpixel(hps[idxs[k]], hps[idxs[k + 1]], pred_fdx, pred_fdy, search_rx=24, search_ry=75, ds=2)
            if m is None:
                continue
            fdx, fdy, corr, pstd = m
            if corr >= min_corr and pstd >= 2.8:
                meas_dx = -fdx
                meas_dy = -fdy
                # Reject wild outliers (> 65 px off predicted pose step)
                if abs(meas_dx - (x0[k + 1] - x0[k])) < 22.0 and abs(meas_dy - (y0[k + 1] - y0[k])) < 65.0:
                    opt_edges.append((k, k + 1, meas_dx, meas_dy, 150.0 * (corr ** 2)))
                    intra_residuals_y.append(meas_dy - (y0[k + 1] - y0[k]))
        xs, ys = _solve_strip_positions(x0, y0, opt_edges, pose_weight=1.0)
        pos_x[idxs] = xs
        pos_y[idxs] = ys
        strip_textured_counts[sid] = len(opt_edges)
        max_shift_y = float(np.max(np.abs(ys - y0)))
        max_shift_x = float(np.max(np.abs(xs - x0)))
        print(f"  Strip {sid}: {len(idxs):3d} frames, {len(opt_edges):3d} optical edges, "
              f"max adjustment vs stage pose: dX={max_shift_x:.1f} px, dY={max_shift_y:.1f} px")

    if intra_residuals_y:
        arr = np.array(intra_residuals_y)
        print(f"  Step-to-step optical vs stage pose Y diff: rms={np.sqrt(np.mean(arr**2)):.2f} px, p95={np.percentile(np.abs(arr),95):.2f} px")

    # Step 2: Inter-strip global column alignment
    print("\n--- Step 2: Inter-strip overlap alignment ---")
    n_strips = len(seg_ids)
    sid_to_idx = {sid: i for i, sid in enumerate(seg_ids)}
    seam_edges: list[tuple[int, int, float, float, float]] = []

    for i in range(n_strips - 1):
        s1, s2 = seg_ids[i], seg_ids[i + 1]
        id1, id2 = segs[s1], segs[s2]
        # Estimate nominal overlap width from strip mean X difference
        nom_dx = float(np.median(pos_x[id2]) - np.median(pos_x[id1]))
        ov_w = int(round(W - nom_dx))
        if ov_w < 80 or ov_w >= W - 64:
            print(f"  Strip {s1}->{s2}: unexpected overlap {ov_w} px, skipping")
            continue

        # Pick textured frames in strip s1 along the right overlap band
        px0 = max(W - ov_w + 24, W - 260)
        px1 = W - 20
        r_stds = np.array([float(hps[idx][180:H - 180, px0:px1].std()) for idx in id1])
        cands = np.where(r_stds > 4.0)[0]
        if len(cands) == 0:
            print(f"  Strip {s1}->{s2}: blank overlap (max std={r_stds.max():.1f}), keeping stage pose")
            continue

        picks = cands[np.linspace(0, len(cands) - 1, min(16, len(cands))).astype(int)]
        obs_dx_list: list[float] = []
        obs_dy_list: list[float] = []
        obs_corr_list: list[float] = []
        y2_arr = pos_y[id2]

        for k1 in picks:
            f1 = id1[k1]
            y1_cur = pos_y[f1]
            x1_cur = pos_x[f1]
            k2_c = int(np.argmin(np.abs(y2_arr - y1_cur)))
            patch = hps[f1][180:H - 180, px0:px1]
            pw = px1 - px0
            ph = (H - 180) - 180
            best = None

            for k2 in range(max(0, k2_c - 5), min(len(id2), k2_c + 6)):
                f2 = id2[k2]
                pred_fdx = x1_cur - pos_x[f2]
                pred_fdy = y1_cur - pos_y[f2]
                ex0 = int(round(px0 + pred_fdx))
                ey0 = int(round(180 + pred_fdy))
                sx0 = max(0, ex0 - 80)
                sy0 = max(0, ey0 - 90)
                sx1 = min(W, ex0 + pw + 80)
                sy1 = min(H, ey0 + ph + 90)
                if sx1 - sx0 <= pw + 4 or sy1 - sy0 <= ph + 4:
                    continue
                res = cv2.matchTemplate(hps[f2][sy0:sy1, sx0:sx1], patch, cv2.TM_CCOEFF_NORMED)
                _, mv, _, ml = cv2.minMaxLoc(res)
                if best is None or mv > best[0]:
                    sub_x, sub_y = _subpixel_peak(res, ml[0], ml[1])
                    fdx = (sx0 + sub_x) - px0
                    fdy = (sy0 + sub_y) - 180.0
                    # Required strip offset (delta_s2 - delta_s1) = (pos_1 - pos_2) - feat_shift
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

    # Solve global strip offsets (delta_x, delta_y) anchored on the most textured strip
    anchor_strip = max(range(n_strips), key=lambda i: strip_textured_counts[seg_ids[i]])
    Sx = np.zeros((n_strips, n_strips), dtype=np.float64)
    bx_s = np.zeros(n_strips, dtype=np.float64)
    Sy = np.zeros((n_strips, n_strips), dtype=np.float64)
    by_s = np.zeros(n_strips, dtype=np.float64)

    # Weak relative stage prior (keeps blank strips at their nominal stage step)
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

    # Step 3: Subpixel center-weighted feather blending
    print("\n--- Step 3: Subpixel feather blending ---")
    out_w = max(1, int(np.ceil((x_max - x_min) * scale_x)))
    out_h = max(1, int(np.ceil((y_max - y_min) * scale_y)))
    accum_bgr = np.zeros((out_h, out_w, 3), dtype=np.float32)
    accum_w = np.zeros((out_h, out_w), dtype=np.float32)
    base_weight = _make_feather_weight(H, W)

    blended_count = 0
    for sid in seg_ids:
        idxs = segs[sid]
        # Always include first and last frame of each strip, plus every `frame_stride`-th frame
        chosen = idxs[::max(1, frame_stride)]
        if idxs[-1] != chosen[-1]:
            chosen = chosen + [idxs[-1]]

        for f_idx in chosen:
            x_f = float(pos_x[f_idx])
            y_f = float(pos_y[f_idx])
            ix = int(np.floor(x_f))
            iy = int(np.floor(y_f))
            fx = x_f - ix
            fy = y_f - iy

            # Bounding box on canvas for (W+1, H+1) subpixel-shifted tile
            tw, th = W + 1, H + 1
            cx0 = max(0, ix)
            cy0 = max(0, iy)
            cx1 = min(out_w, ix + tw)
            cy1 = min(out_h, iy + th)
            if cx1 <= cx0 or cy1 <= cy0:
                continue

            img_f = frames_bgr[f_idx].astype(np.float32)
            if gain_map is not None:
                img_f *= gain_map

            # Subpixel shift into (tw, th) tile
            M = np.array([[1.0, 0.0, fx], [0.0, 1.0, fy]], dtype=np.float32)
            shifted_bgr = cv2.warpAffine(img_f, M, (tw, th), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0))
            shifted_w = cv2.warpAffine(base_weight, M, (tw, th), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0.0)

            sx0 = cx0 - ix
            sy0 = cy0 - iy
            sx1 = cx1 - ix
            sy1 = cy1 - iy

            w_crop = shifted_w[sy0:sy1, sx0:sx1]
            accum_bgr[cy0:cy1, cx0:cx1] += shifted_bgr[sy0:sy1, sx0:sx1] * w_crop[:, :, None]
            accum_w[cy0:cy1, cx0:cx1] += w_crop
            blended_count += 1

    valid = accum_w > 1e-6
    out_img = np.full((out_h, out_w, 3), 255, dtype=np.uint8)
    out_img[valid] = np.clip(accum_bgr[valid] / accum_w[valid, None], 0.0, 255.0).astype(np.uint8)

    if output_path is None:
        output_path = scan_dir / "scan_composite_stitched.tif"

    # Save uncompressed/LZW TIFF
    ok = cv2.imwrite(str(output_path), out_img, [cv2.IMWRITE_TIFF_COMPRESSION, 5])
    if not ok:
        ok = cv2.imwrite(str(output_path), out_img)
    if not ok:
        raise RuntimeError(f"Failed to write {output_path}")

    elapsed = time.perf_counter() - t0
    print(f"Saved {output_path} ({out_w}x{out_h}, {blended_count} frames blended, "
          f"coverage={np.mean(valid)*100:.2f}%, {output_path.stat().st_size/1e6:.2f} MB) in {elapsed:.1f}s")

    # Optional debug crops comparing raw scan_composite.tif vs stitched output
    if save_debug_dir is not None:
        save_debug_dir.mkdir(parents=True, exist_ok=True)
        cv2.imwrite(str(save_debug_dir / f"{scan_dir.name}_stitched_overview.png"),
                    cv2.resize(out_img, (out_w // 8, out_h // 8), interpolation=cv2.INTER_AREA))

        # Locate circle center in red channel for zoomed ruler & circle-top crops
        R = out_img[:, :, 2]
        mid_y = R[2700:3500, 2200:min(out_w, 6800)]
        col_dark = np.mean(mid_y < 95, axis=0)
        lxs = np.where(col_dark[:1500] > 0.25)[0] + 2200
        rxs = np.where(col_dark[2500:] > 0.25)[0] + 2200 + 2500
        if len(lxs) and len(rxs):
            cx = int(round(0.5 * (float(np.median(lxs)) + float(np.median(rxs)))))
            mid_x = R[900:min(out_h, 5500), max(0, cx - 250):max(50, cx - 100)]
            row_dark = np.mean(mid_x < 95, axis=1)
            tys = np.where(row_dark[:1500] > 0.25)[0] + 900
            bys = np.where(row_dark[2500:] > 0.25)[0] + 900 + 2500
            if len(tys) and len(bys):
                cy_t = int(round(float(np.median(tys))))
                cy_b = int(round(float(np.median(bys))))
                cy = (cy_t + cy_b) // 2
                diam_x = float(np.median(rxs)) - float(np.median(lxs))
                diam_y = float(cy_b - cy_t)
                print(f"Stitched circle: W={diam_x:.1f} px, H={diam_y:.1f} px, aspect W/H={diam_x/diam_y:.4f}")
                cv2.imwrite(str(save_debug_dir / f"{scan_dir.name}_stitched_zoom_top.png"),
                            out_img[max(0, cy_t - 150):cy_t + 300, max(0, cx - 200):min(out_w, cx + 650)])
                cv2.imwrite(str(save_debug_dir / f"{scan_dir.name}_stitched_zoom_ruler.png"),
                            out_img[max(0, cy - 220):min(out_h, cy + 220), max(0, cx - 600):min(out_w, cx + 600)])

    return output_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Optical subpixel stitcher for microscope scan directories.")
    parser.add_argument("scan_dir", type=Path, help="Path to scan directory containing scan_capture.mkv and scan_metadata.json.")
    parser.add_argument("-o", "--output", type=Path, default=None, help="Output TIFF path (default: <scan_dir>/scan_composite_stitched.tif).")
    parser.add_argument("--no-flatfield", action="store_true", help="Disable smooth illumination flat-field normalization.")
    parser.add_argument("--stride", type=int, default=1, help="Blend every Nth frame per strip (default: 1 = all frames).")
    parser.add_argument("--min-corr", type=float, default=0.72, help="Minimum NCC correlation threshold (default: 0.72).")
    parser.add_argument("--debug-dir", type=Path, default=None, help="Optional directory to save overview and zoom verification PNGs.")
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
