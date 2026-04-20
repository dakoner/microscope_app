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

import _microscope_app_cpp as native


def main() -> int:
    app = QApplication(sys.argv)

    window_ptr = native.create_main_window_ptr()
    main_window = shiboken6.wrapInstance(int(window_ptr), QMainWindow)
    native.show_maximized_ptr(window_ptr)

    exit_code = app.exec()

    # C++ side owns the object lifetime here.
    native.delete_main_window_ptr(window_ptr)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
