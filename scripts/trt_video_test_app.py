#!/usr/bin/env python3
"""Simple TensorRT YOLO video viewer with live annotations.

Usage examples:
  /home/davidek/src/microtools/microscope_app/.venv/bin/python trt_video_test_app.py
  /home/davidek/src/microtools/microscope_app/.venv/bin/python trt_video_test_app.py --source videos/scan_1776551490/your_video.mp4
  /home/davidek/src/microtools/microscope_app/.venv/bin/python trt_video_test_app.py --source 0
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

import cv2
from ultralytics import YOLO


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run TensorRT YOLO on a video stream and display annotations.")
    parser.add_argument(
        "--engine",
        default="models/train13/weights/best.engine",
        help="Path to TensorRT engine file (default: models/train13/weights/best.engine)",
    )
    parser.add_argument(
        "--source",
        default="0",
        help="Video source. Use camera index (e.g. 0) or a video file path.",
    )
    parser.add_argument("--imgsz", type=int, default=640, help="Inference size.")
    parser.add_argument("--conf", type=float, default=0.25, help="Confidence threshold.")
    parser.add_argument("--iou", type=float, default=0.45, help="NMS IoU threshold.")
    parser.add_argument("--device", default="0", help="Inference device (e.g. 0, cuda:0, cpu).")
    parser.add_argument("--window", default="TensorRT YOLO Test", help="Display window title.")
    return parser


def parse_source(source_arg: str) -> int | str:
    if source_arg.isdigit():
        return int(source_arg)
    return source_arg


def main() -> int:
    args = build_arg_parser().parse_args()

    engine_path = pathlib.Path(args.engine).expanduser().resolve()
    if not engine_path.exists():
        print(f"ERROR: TensorRT engine not found: {engine_path}", file=sys.stderr)
        return 1

    source = parse_source(args.source)
    if isinstance(source, str):
        source_path = pathlib.Path(source).expanduser()
        if not source_path.exists():
            print(f"ERROR: Video source not found: {source_path}", file=sys.stderr)
            return 1
        source = str(source_path.resolve())

    print(f"Loading TensorRT engine: {engine_path}")
    model = YOLO(str(engine_path), task="detect")

    cap = cv2.VideoCapture(source)
    if not cap.isOpened():
        print(f"ERROR: Failed to open source: {args.source}", file=sys.stderr)
        return 1

    print("Press 'q' or ESC to quit.")

    frame_count = 0
    start_time = time.perf_counter()

    while True:
        ok, frame = cap.read()
        if not ok:
            break

        frame_count += 1

        results = model.predict(
            source=frame,
            conf=args.conf,
            iou=args.iou,
            imgsz=args.imgsz,
            device=args.device,
            verbose=False,
        )

        result = results[0]
        annotated = result.plot()

        elapsed = max(time.perf_counter() - start_time, 1e-6)
        avg_fps = frame_count / elapsed
        num_dets = 0 if result.boxes is None else len(result.boxes)

        overlay = f"frame={frame_count} dets={num_dets} avg_fps={avg_fps:.2f}"
        cv2.putText(
            annotated,
            overlay,
            (20, 35),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.9,
            (0, 255, 255),
            2,
            cv2.LINE_AA,
        )

        cv2.imshow(args.window, annotated)
        key = cv2.waitKey(1) & 0xFF
        if key == ord("q") or key == 27:
            break

    cap.release()
    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
