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

from PySide6.QtCore import QTimer
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
        # 11.x zips keep them in bin/, 10.x zips in lib/.
        third_party = Path(__file__).resolve().parent / "third_party"
        for pattern in ("TensorRT-*/bin", "TensorRT-*/lib"):
            for trt_dir in sorted(third_party.glob(pattern)):
                os.add_dll_directory(str(trt_dir))


_configure_native_module_path()

try:
    import _microscope_app_cpp as native
except ModuleNotFoundError as exc:
    raise ModuleNotFoundError(
        "The native microscope module has not been built. Run `make` first, "
        "then launch with `uv run run_microscope_app.py`."
    ) from exc
except ImportError as exc:
    # The extension links against one specific CPython and Qt runtime, so a
    # virtual environment recreated with a different Python minor version (or a
    # different PySide6) shows up as an opaque DLL load failure on Windows.
    raise ImportError(
        "The native microscope module was built against a different Python or "
        r"Qt runtime. Rebuild it in this environment: `scripts\build_windows.ps1 "
        "-WithYolo -Clean` on Windows, `make` elsewhere."
    ) from exc

from microscope_mcp import McpMode, QtMicroscopeBridge, start_local_mcp_server


def _apply_cli_options(argv: list[str]) -> tuple[list[str], argparse.Namespace]:
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
        action=argparse.BooleanOptionalAction,
        default=None,
        help="Connect the CNC stage on startup.  Defaults to the 'stage_auto_connect' "
             "entry in camera_settings.json; --no-auto-connect-stage overrides it for one run.",
    )
    parser.add_argument(
        "--mcp-mode",
        choices=("auto", "full", "app"),
        default=os.environ.get("MICROSCOPE_MCP_MODE", "auto").lower(),
        help="MCP tool profile: 'auto' uses 'app' in remote mode and 'full' in local mode.",
    )
    parser.add_argument(
        "--mcp-port",
        type=int,
        default=None,
        help="Port for the GUI MCP server (default: 8767 in 'app' mode, 8765 in 'full' mode).",
    )
    parser.add_argument(
        "--mcp-bind",
        default=None,
        help="Bind address for the GUI MCP server (default: MICROSCOPE_MCP_BIND or 127.0.0.1).",
    )
    parser.add_argument(
        "--no-mcp",
        action="store_true",
        help="Disable the embedded GUI MCP server.",
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
    if known.auto_connect_stage is not None:
        os.environ["MICROSCOPE_STAGE_AUTO_CONNECT"] = "1" if known.auto_connect_stage else "0"

    return [argv[0], *remaining], known


def _resolve_mcp_mode(requested: str, bridge: QtMicroscopeBridge) -> McpMode:
    if requested in {"full", "app"}:
        return requested  # type: ignore[return-value]
    status = bridge.call("status")
    return "app" if status.get("camera_mode") == "remote" else "full"


def _start_mcp_server(
    bridge: QtMicroscopeBridge, opts: argparse.Namespace
) -> None:
    start_local_mcp_server(
        bridge,
        mode=_resolve_mcp_mode(opts.mcp_mode, bridge),
        port=opts.mcp_port,
        host=opts.mcp_bind,
        enabled=False if opts.no_mcp else None,
    )


def main() -> int:
    qt_argv, opts = _apply_cli_options(sys.argv)
    app = QApplication(qt_argv)

    window_ptr = native.create_main_window_ptr()
    main_window = shiboken6.wrapInstance(int(window_ptr), QMainWindow)
    native.show_maximized_ptr(window_ptr)
    mcp_bridge = QtMicroscopeBridge(native, int(window_ptr), target_kind="window")
    app.aboutToQuit.connect(mcp_bridge.close)
    # MainWindow applies camera_settings.json (local vs remote camera_server) from
    # its own zero-delay timer, so the tool profile can only be resolved once the
    # event loop has run that timer; starting earlier would always report "local"
    # and bind the combined profile on the hardware port.
    QTimer.singleShot(0, lambda: _start_mcp_server(mcp_bridge, opts))

    exit_code = app.exec()

    # C++ side owns the object lifetime here.
    mcp_bridge.close()
    native.delete_main_window_ptr(window_ptr)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
