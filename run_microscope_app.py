#!/usr/bin/env python3
from __future__ import annotations

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

    if plugins_dir.is_dir():
        os.environ.setdefault("QT_PLUGIN_PATH", str(plugins_dir))


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


_configure_native_module_path()

try:
    import _microscope_app_cpp as native
except ModuleNotFoundError as exc:
    raise ModuleNotFoundError(
        "The native microscope module has not been built. Run `make` first, "
        "then launch with `uv run run_microscope_app.py`."
    ) from exc

from microscope_mcp import QtMicroscopeBridge, start_local_mcp_server


def main() -> int:
    app = QApplication(sys.argv)

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
