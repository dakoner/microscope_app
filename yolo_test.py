#!/usr/bin/env python3
"""
Test program for real-time YOLO inference on video files.

Reads frames from a video file, sends them to yolo_inference.py for inference,
and displays results with bounding boxes in real-time.

Usage:
    python3 yolo_test.py <video_file> [--conf 0.5]
"""

import sys
import json
import base64
import subprocess
import argparse
from pathlib import Path

import cv2
import numpy as np


def main():
    parser = argparse.ArgumentParser(description="Test YOLO inference on video file")
    parser.add_argument("video_file", help="Path to video file")
    parser.add_argument("--conf", type=float, default=0.5, help="Confidence threshold (default: 0.5)")
    parser.add_argument("--skip", type=int, default=1, help="Process every Nth frame (default: 1)")
    args = parser.parse_args()

    # Validate video file
    video_path = Path(args.video_file)
    if not video_path.exists():
        print(f"Error: Video file not found: {video_path}", file=sys.stderr)
        sys.exit(1)

    # Open video
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"Error: Could not open video file: {video_path}", file=sys.stderr)
        sys.exit(1)

    fps = cap.get(cv2.CAP_PROP_FPS)
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    print(f"Video: {video_path.name}")
    print(f"Resolution: {width}x{height}, FPS: {fps:.1f}, Total frames: {total_frames}")
    print(f"Confidence threshold: {args.conf}")
    print(f"Frame skip: {args.skip} (processing every {args.skip} frame(s))")
    print("\nStarting inference... Press 'q' to quit, 'p' to pause/resume")
    print("-" * 60)

    # Start inference subprocess
    script_path = Path(__file__).parent / "yolo_inference.py"
    if not script_path.exists():
        print(f"Error: yolo_inference.py not found at {script_path}", file=sys.stderr)
        sys.exit(1)

    try:
        proc = subprocess.Popen(
            ["python3", str(script_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )
    except Exception as e:
        print(f"Error: Could not start inference process: {e}", file=sys.stderr)
        sys.exit(1)

    frame_count = 0
    processed_count = 0
    paused = False

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            frame_count += 1

            # Handle frame skipping
            if (frame_count - 1) % args.skip != 0:
                continue

            processed_count += 1

            # Encode frame as JPEG base64
            _, jpeg_bytes = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
            frame_b64 = base64.b64encode(jpeg_bytes).decode("utf-8")

            # Send to inference
            frame_data = {
                "frame": frame_b64,
                "conf_threshold": args.conf,
                "width": width,
                "height": height
            }
            
            try:
                proc.stdin.write(json.dumps(frame_data) + "\n")
                proc.stdin.flush()
            except (BrokenPipeError, OSError):
                print("\nError: Inference process disconnected", file=sys.stderr)
                break

            # Read detections
            try:
                result_line = proc.stdout.readline()
                if not result_line:
                    print("\nError: No response from inference process", file=sys.stderr)
                    break

                result = json.loads(result_line)
                detections = result.get("detections", [])
            except json.JSONDecodeError as e:
                print(f"\nError: Invalid JSON response: {e}", file=sys.stderr)
                detections = []

            # Draw detections
            display_frame = frame.copy()
            for det in detections:
                x, y, w, h = det["x"], det["y"], det["w"], det["h"]
                conf = det["conf"]

                # Draw green bounding box
                cv2.rectangle(display_frame, (x, y), (x + w, y + h), (0, 255, 0), 2)

                # Draw confidence label
                label = f"{conf:.2f}"
                font = cv2.FONT_HERSHEY_SIMPLEX
                font_scale = 0.6
                thickness = 1
                text_size = cv2.getTextSize(label, font, font_scale, thickness)[0]

                # Background for label
                cv2.rectangle(
                    display_frame,
                    (x, y - text_size[1] - 4),
                    (x + text_size[0] + 4, y),
                    (0, 255, 0),
                    -1
                )
                # Label text
                cv2.putText(
                    display_frame,
                    label,
                    (x + 2, y - 2),
                    font,
                    font_scale,
                    (0, 0, 0),
                    thickness
                )

            # Draw info
            info_text = f"Frame: {frame_count}/{total_frames} | Detections: {len(detections)} | Processed: {processed_count}"
            cv2.putText(
                display_frame,
                info_text,
                (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (0, 255, 0),
                2
            )

            # Display
            cv2.imshow("YOLO Inference Test", display_frame)

            # Handle keyboard
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                print("\nQuitting...")
                break
            elif key == ord('p'):
                paused = not paused
                if paused:
                    print("Paused (press 'p' to resume)")
                else:
                    print("Resumed")

            # Show stats periodically
            if processed_count % 30 == 0:
                inf_rate = processed_count / max(1, frame_count) * fps
                print(f"Frame {frame_count}/{total_frames} | Processed: {processed_count} | Inference rate: {inf_rate:.1f} fps")

    except KeyboardInterrupt:
        print("\nInterrupted by user")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
        except Exception:
            pass

        print(f"\nProcessing complete: {processed_count} frames processed out of {frame_count}")


if __name__ == "__main__":
    main()
