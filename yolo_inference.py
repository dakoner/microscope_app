#!/usr/bin/env python3
"""
YOLO Inference Server for real-time tardigrade detection.

This script runs as a subprocess and communicates with the C++ application
via stdin/stdout. It loads a YOLO model and performs inference on received frames.

Protocol:
  Input:  JSON with "frame" (base64-encoded JPEG image), width, height
  Output: JSON with detections [{"x": ..., "y": ..., "w": ..., "h": ..., "conf": ...}]
"""

import sys
import json
import base64
import cv2
import numpy as np
from pathlib import Path

try:
    from ultralytics import YOLO
except ImportError:
    print("Error: ultralytics package not found. Install with: pip install ultralytics", file=sys.stderr)
    sys.exit(1)

# Find model path
MODEL_DIR = Path(__file__).parent / "models" / "train13" / "weights"
MODEL_PATH = MODEL_DIR / "best.pt"

if not MODEL_PATH.exists():
    print(f"Error: Model not found at {MODEL_PATH}", file=sys.stderr)
    sys.exit(1)

# Load YOLO model
try:
    model = YOLO(str(MODEL_PATH))
    print(f"Loaded model from {MODEL_PATH}", file=sys.stderr, flush=True)
except Exception as e:
    print(f"Error loading model: {e}", file=sys.stderr)
    sys.exit(1)

def process_frame(frame_data):
    """
    Process a single frame.
    
    Args:
        frame_data: dict with "frame" (base64 JPEG), optional "conf_threshold"
    
    Returns:
        list of detections: [{"x": ..., "y": ..., "w": ..., "h": ..., "conf": ...}]
    """
    try:
        # Decode base64 image
        frame_b64 = frame_data.get("frame")
        if not frame_b64:
            return []
        
        frame_bytes = base64.b64decode(frame_b64)
        frame_array = np.frombuffer(frame_bytes, dtype=np.uint8)
        frame = cv2.imdecode(frame_array, cv2.IMREAD_COLOR)
        
        if frame is None:
            return []
        
        # Run inference
        conf_threshold = frame_data.get("conf_threshold", 0.5)
        results = model.predict(frame, conf=conf_threshold, verbose=False)
        
        detections = []
        for result in results:
            if result.boxes is not None:
                for box in result.boxes:
                    x1, y1, x2, y2 = (box.xyxy[0].cpu().numpy() if hasattr(box.xyxy[0], 'cpu') 
                                      else box.xyxy[0]).astype(int)
                    conf = float(box.conf.cpu().item() if hasattr(box.conf, 'cpu') else box.conf.item())
                    
                    w = int(x2 - x1)
                    h = int(y2 - y1)
                    x = int(x1)
                    y = int(y1)
                    
                    detections.append({
                        "x": x,
                        "y": y,
                        "w": w,
                        "h": h,
                        "conf": round(conf, 3)
                    })
        
        # Log inference result to console
        print(f"[YOLO] Inference complete: {len(detections)} detection(s)", file=sys.stderr, flush=True)
        
        return detections
    
    except Exception as e:
        print(f"[YOLO] Error processing frame: {e}", file=sys.stderr, flush=True)
        return []

def main():
    """Main inference loop."""
    print("YOLO Inference Server started", file=sys.stderr, flush=True)
    sys.stderr.flush()
    
    frame_count = 0
    
    while True:
        try:
            # Read JSON line from stdin
            line = sys.stdin.readline()
            if not line:
                break
            
            frame_count += 1
            print(f"[YOLO] Received frame #{frame_count}", file=sys.stderr, flush=True)
            
            frame_data = json.loads(line.strip())
            detections = process_frame(frame_data)
            
            # Send detections as JSON
            result = {"detections": detections}
            print(json.dumps(result), flush=True)
            sys.stdout.flush()
        
        except json.JSONDecodeError as e:
            print(f"[YOLO] Error decoding JSON: {e}", file=sys.stderr, flush=True)
            continue
        except Exception as e:
            print(f"[YOLO] Error in main loop: {e}", file=sys.stderr, flush=True)
            continue

if __name__ == "__main__":
    main()
