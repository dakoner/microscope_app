#!/usr/bin/env python3
"""Headless network server for the MindVision microscope camera.

Collects image frames from the MindVision camera (or synthetic test generator)
on a headless host, streams compressed JPEG or uncompressed RGB888 frames over
TCP, and accepts remote control commands (exposure, gain, auto-exposure, ROI,
triggers, strobe, and server-side video recording).
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import sys


def _configure_qt_runtime_from_pyside6() -> None:
    import PySide6

    pyside_root = Path(PySide6.__file__).resolve().parent
    qt_root = pyside_root / "Qt"
    plugins_dir = qt_root / "plugins"
    if plugins_dir.is_dir():
        os.environ.setdefault("QT_PLUGIN_PATH", str(plugins_dir))


_configure_qt_runtime_from_pyside6()

from PySide6.QtCore import QCoreApplication, QTimer


def _configure_native_module_path() -> None:
    root = Path(__file__).resolve().parent
    for rel in ("release", "src/mindvision_qobject/release"):
        candidate = root / rel
        if candidate.is_dir():
            sys.path.insert(0, str(candidate))


_configure_native_module_path()


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the headless MindVision camera TCP server."
    )
    parser.add_argument(
        "-b",
        "--bind",
        "--host",
        dest="bind",
        default=os.environ.get("MICROSCOPE_CAMERA_BIND", "0.0.0.0"),
        help="Address to bind the TCP server to (default: 0.0.0.0).",
    )
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=int(os.environ.get("MICROSCOPE_CAMERA_PORT", "8766")),
        help="TCP port to listen on (default: 8766).",
    )
    parser.add_argument(
        "-q",
        "--jpeg-quality",
        type=int,
        default=int(os.environ.get("MICROSCOPE_CAMERA_JPEG_QUALITY", "85")),
        help="Default JPEG quality (1..100, or 0 for raw RGB888; default: 85).",
    )
    parser.add_argument(
        "-r",
        "--record-dir",
        default=os.environ.get("MICROSCOPE_CAMERA_RECORD_DIR", "videos"),
        help="Directory for server-side video recordings (default: videos).",
    )
    parser.add_argument(
        "--auto-start",
        action="store_true",
        default=os.environ.get("MICROSCOPE_CAMERA_AUTO_START", "0").lower() in {"1", "true", "yes"},
        help="Open and start camera capture immediately on server startup.",
    )
    parser.add_argument(
        "--keep-open",
        action="store_true",
        default=os.environ.get("MICROSCOPE_CAMERA_KEEP_OPEN", "0").lower() in {"1", "true", "yes"},
        help="Keep the camera open/running when all clients disconnect.",
    )
    parser.add_argument(
        "--synthetic",
        action="store_true",
        default=os.environ.get("MICROSCOPE_CAMERA_SYNTHETIC", "0").lower() in {"1", "true", "yes"},
        help="Use synthetic test-pattern camera and synthetic stage when no physical hardware is attached.",
    )
    parser.add_argument(
        "--stage-port",
        default=os.environ.get("MICROSCOPE_STAGE_PORT", ""),
        help="Optional serial port for the CNC stage controller to open on startup (e.g. ttyUSB0 or SYNTHETIC_STAGE).",
    )
    parser.add_argument(
        "--stage-baud",
        type=int,
        default=int(os.environ.get("MICROSCOPE_STAGE_BAUD", "115200")),
        help="Baud rate for the CNC stage serial port (default: 115200).",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)

    try:
        import _microscope_app_cpp as native
    except ModuleNotFoundError as exc:
        raise ModuleNotFoundError(
            "The native microscope module has not been built. Run `make` first, "
            "then launch with `uv run run_camera_server.py`."
        ) from exc

    app = QCoreApplication(sys.argv[:1])

    # Allow clean Ctrl+C / SIGTERM shutdown inside Qt's C++ event loop
    signal.signal(signal.SIGINT, lambda *_: app.quit())
    signal.signal(signal.SIGTERM, lambda *_: app.quit())
    sig_timer = QTimer()
    sig_timer.start(200)
    sig_timer.timeout.connect(lambda: None)

    server_ptr = native.create_camera_server_ptr(
        args.bind,
        args.port,
        args.jpeg_quality,
        args.record_dir,
        args.auto_start,
        args.keep_open or args.auto_start,
        args.synthetic,
        args.stage_port,
        args.stage_baud,
    )
    if not server_ptr:
        return 1

    try:
        return app.exec()
    finally:
        native.delete_camera_server_ptr(server_ptr)


if __name__ == "__main__":
    raise SystemExit(main())
