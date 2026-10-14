from PySide6 import QtCore


TARGET_X_MM = 100.0
TARGET_Y_MM = 100.0
FEEDRATE_MM_PER_MIN = 500


cnc = app.find_child("m_cncControlPanel")
if cnc is None:
    raise RuntimeError("Could not find m_cncControlPanel. Open the main microscope window first.")


def send(cmd: str) -> None:
    ok = QtCore.QMetaObject.invokeMethod(
        cnc,
        "sendCommand",
        QtCore.Qt.DirectConnection,
        QtCore.Q_ARG(str, cmd),
    )
    if not ok:
        raise RuntimeError(f"Failed to send CNC command: {cmd}")


print(f"Moving stage to X={TARGET_X_MM:.3f}, Y={TARGET_Y_MM:.3f} at F{FEEDRATE_MM_PER_MIN}")
send("G90")
send(f"G1 X{TARGET_X_MM:.3f} Y{TARGET_Y_MM:.3f} F{FEEDRATE_MM_PER_MIN}")
