#!/usr/bin/env python3
import os
import sys

REQUIRED_MM = (3, 11)
PY311 = "/home/davidek/.local/share/uv/python/cpython-3.11-linux-x86_64-gnu/bin/python3.11"

if sys.version_info[:2] != REQUIRED_MM and os.path.exists(PY311):
    os.execv(PY311, [PY311, *sys.argv])

# Hosted mode tells MainWindow to skip embedded CPython init/finalize.
os.environ["MICROSCOPE_PY_HOSTED"] = "1"

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
