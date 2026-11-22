#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import os
import sys

# Hosted mode tells MainWindow to skip embedded CPython init/finalize.
os.environ["MICROSCOPE_PY_HOSTED"] = "1"


def _configure_qt_runtime_from_pyside6() -> None:
    # Resolve PySide6's bundled Qt locations and prefer them for plugins.
    import PySide6

    pyside_root = Path(PySide6.__file__).resolve().parent
    qt_root = pyside_root / "Qt"
    plugins_dir = qt_root / "plugins"
    if not plugins_dir.is_dir():
        # Windows wheels keep the Qt runtime directly inside the package.
        plugins_dir = pyside_root / "plugins"

    if plugins_dir.is_dir():
        os.environ.setdefault("QT_PLUGIN_PATH", str(plugins_dir))

    if sys.platform.startswith("win"):
        # Make the Qt DLLs importable by the native C++ extension module.
        for dll_dir in (qt_root / "bin", pyside_root):
            if dll_dir.is_dir():
                os.add_dll_directory(str(dll_dir))


_configure_qt_runtime_from_pyside6()

from PySide6.QtWidgets import QApplication, QMainWindow
import shiboken6


def _configure_native_module_path() -> None:
    """Expose the qmake build output to the Python launcher.

    qmake writes the pybind extension to ``release/``.  ``uv run`` executes
    this source file from the project directory, so Python would otherwise not
    search that sibling build directory.
    """
    release_dir = Path(__file__).resolve().parent / "release"
    if release_dir.is_dir():
        sys.path.insert(0, str(release_dir))
        if sys.platform.startswith("win"):
            # Native DLLs shipped next to the extension module.
            os.add_dll_directory(str(release_dir))

    if sys.platform.startswith("win"):
        # The TensorRT DLLs are hundreds of MB, so they stay in the unpacked
        # NVIDIA zip under third_party/ and are added to the DLL search path.
        third_party = Path(__file__).resolve().parent / "third_party"
        for trt_lib in sorted(third_party.glob("TensorRT-*/lib")):
            os.add_dll_directory(str(trt_lib))


_configure_native_module_path()

try:
    import _microscope_app_cpp as native
except ModuleNotFoundError as exc:
    raise ModuleNotFoundError(
        "The native microscope module has not been built. Run `make` first, "
        "then launch with `uv run run_microscope_app.py`."
    ) from exc

from microscope_mcp import QtMicroscopeBridge, start_local_mcp_server


def _apply_cli_camera_options(argv: list[str]) -> list[str]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument(
        "--remote-camera",
        "--remote",
        "--remote-stage",
        dest="remote_camera",
        metavar="HOST[:PORT]",
        help="Connect camera and stage to a remote camera_server (e.g. 192.168.1.50:8766).",
    )
    parser.add_argument(
        "--camera-mode",
        choices=("local", "remote"),
        help="Select local hardware or remote TCP camera_server.",
    )
    parser.add_argument("--camera-host", help="Remote camera_server hostname or IP.")
    parser.add_argument("--camera-port", type=int, help="Remote camera_server TCP port.")
    parser.add_argument(
        "--camera-jpeg-quality",
        type=int,
        help="Remote stream JPEG quality (1..100, or 0 for uncompressed raw RGB888).",
    )
    parser.add_argument(
        "--camera-remote-record",
        action="store_true",
        help="Record videos on the remote camera host instead of the local client.",
    )
    parser.add_argument(
        "--synthetic-camera",
        action="store_true",
        help="Use synthetic test-pattern camera when in local mode.",
    )
    parser.add_argument(
        "--stage-port",
        help="Serial port name to connect the CNC stage controller to (e.g. ttyUSB0 or SYNTHETIC_STAGE).",
    )
    parser.add_argument(
        "--auto-connect-stage",
        action="store_true",
        help="Automatically connect to the CNC stage serial port on startup.",
    )
    known, remaining = parser.parse_known_args(argv[1:])

    if known.remote_camera:
        endpoint = known.remote_camera.strip()
        if ":" in endpoint:
            host, port_str = endpoint.rsplit(":", 1)
            os.environ["MICROSCOPE_CAMERA_HOST"] = host
            os.environ["MICROSCOPE_CAMERA_PORT"] = port_str
        else:
            os.environ["MICROSCOPE_CAMERA_HOST"] = endpoint
        os.environ["MICROSCOPE_CAMERA_MODE"] = "remote"

    if known.camera_mode:
        os.environ["MICROSCOPE_CAMERA_MODE"] = known.camera_mode
    if known.camera_host:
        os.environ["MICROSCOPE_CAMERA_HOST"] = known.camera_host
    if known.camera_port is not None:
        os.environ["MICROSCOPE_CAMERA_PORT"] = str(known.camera_port)
    if known.camera_jpeg_quality is not None:
        os.environ["MICROSCOPE_CAMERA_JPEG_QUALITY"] = str(known.camera_jpeg_quality)
    if known.camera_remote_record:
        os.environ["MICROSCOPE_CAMERA_REMOTE_RECORD"] = "1"
    if known.synthetic_camera:
        os.environ["MICROSCOPE_CAMERA_SYNTHETIC"] = "1"
    if known.stage_port:
        os.environ["MICROSCOPE_STAGE_PORT"] = known.stage_port
    if known.auto_connect_stage:
        os.environ["MICROSCOPE_STAGE_AUTO_CONNECT"] = "1"

    return [argv[0], *remaining]


def main() -> int:
    qt_argv = _apply_cli_camera_options(sys.argv)
    app = QApplication(qt_argv)

    window_ptr = native.create_main_window_ptr()
    main_window = shiboken6.wrapInstance(int(window_ptr), QMainWindow)
    native.show_maximized_ptr(window_ptr)
    mcp_bridge = QtMicroscopeBridge(native, int(window_ptr))
    app.aboutToQuit.connect(mcp_bridge.close)
    start_local_mcp_server(mcp_bridge)

    exit_code = app.exec()

    # C++ side owns the object lifetime here.
    mcp_bridge.close()
    native.delete_main_window_ptr(window_ptr)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
