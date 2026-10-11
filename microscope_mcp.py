"""Local MCP server for the microscope application.

The server intentionally listens only on loopback.  Its tools are invoked on
the Qt GUI thread through ``QtMicroscopeBridge``; the MCP worker never calls
the native hardware/UI objects directly.
"""

from __future__ import annotations

import json
import logging
import os
import threading
from dataclasses import dataclass
from typing import Any, Callable

from PySide6.QtCore import QObject, QThread, Qt, Signal, Slot


LOG = logging.getLogger(__name__)


@dataclass
class _PendingCall:
    operation: str
    args: tuple[Any, ...]
    done: threading.Event
    result: str | None = None
    error: BaseException | None = None


class QtMicroscopeBridge(QObject):
    """Synchronously marshal a request from an MCP worker onto the Qt thread."""

    _requested = Signal(object)

    def __init__(self, native_module: Any, window_ptr: int) -> None:
        super().__init__()
        self._native = native_module
        self._window_ptr = window_ptr
        self._closed = False
        self._requested.connect(self._execute, Qt.QueuedConnection)

    def close(self) -> None:
        self._closed = True

    def call(self, operation: str, *args: Any, timeout_seconds: float = 15) -> dict[str, Any]:
        if self._closed:
            raise RuntimeError("The microscope application is closing.")

        # Useful for direct tests and avoids a deadlock if a call ever originates
        # from the GUI thread.
        if QThread.currentThread() == self.thread():
            return self._call_native(operation, args)

        pending = _PendingCall(operation, args, threading.Event())
        self._requested.emit(pending)
        if not pending.done.wait(timeout_seconds):
            raise TimeoutError("The microscope application did not answer the MCP request in time.")
        if pending.error:
            raise pending.error
        if pending.result is None:
            raise RuntimeError("The microscope application returned no result.")
        return json.loads(pending.result)

    @Slot(object)
    def _execute(self, pending: _PendingCall) -> None:
        try:
            pending.result = json.dumps(self._call_native(pending.operation, pending.args))
        except BaseException as exc:  # communicate a native error back to the MCP worker
            pending.error = exc
        finally:
            pending.done.set()

    def _call_native(self, operation: str, args: tuple[Any, ...]) -> dict[str, Any]:
        methods: dict[str, Callable[..., str]] = {
            "status": lambda: self._native.mcp_status_ptr(self._window_ptr),
            "start_camera": lambda: self._native.mcp_start_camera_ptr(self._window_ptr),
            "stop_camera": lambda: self._native.mcp_stop_camera_ptr(self._window_ptr),
            "snapshot": lambda: self._native.mcp_snapshot_ptr(self._window_ptr),
            "recording": lambda: self._native.mcp_set_recording_ptr(self._window_ptr, *args),
            "exposure": lambda: self._native.mcp_set_exposure_ptr(self._window_ptr, *args),
            "gain": lambda: self._native.mcp_set_gain_ptr(self._window_ptr, *args),
            "auto_exposure": lambda: self._native.mcp_set_auto_exposure_ptr(self._window_ptr, *args),
            "yolo": lambda: self._native.mcp_set_yolo_ptr(self._window_ptr, *args),
            "move_stage": lambda: self._native.mcp_move_stage_ptr(self._window_ptr, *args),
            "emergency_stop_stage": lambda: self._native.mcp_emergency_stop_stage_ptr(self._window_ptr),
        }
        try:
            response = methods[operation]()
        except KeyError as exc:
            raise ValueError(f"Unsupported microscope MCP operation: {operation}") from exc
        return json.loads(response)


def create_server(bridge: QtMicroscopeBridge, *, port: int) -> Any:
    """Create an MCP server without starting a listener.

    Importing MCP here keeps the regular microscope UI usable when optional
    MCP dependencies have not been installed yet.
    """
    from mcp.server.fastmcp import FastMCP

    server = FastMCP(
        "microscope-app",
        instructions=(
            "Control a local microscope application. Inspect status before changing "
            "hardware. Never move the stage unless the user explicitly confirms the "
            "destination and the move_stage tool's confirm argument is true."
        ),
        # FastMCP 1.x receives listener configuration at construction time;
        # run() only receives the transport selection.
        host="127.0.0.1",
        port=port,
        streamable_http_path="/mcp",
    )

    @server.tool()
    def get_microscope_status() -> dict[str, Any]:
        """Get camera, recording, AI inference, and stage position/state."""
        return bridge.call("status")

    @server.tool()
    def start_camera() -> dict[str, Any]:
        """Open and start the microscope camera stream."""
        return bridge.call("start_camera")

    @server.tool()
    def stop_camera() -> dict[str, Any]:
        """Stop and close the microscope camera stream."""
        return bridge.call("stop_camera")

    @server.tool()
    def capture_snapshot() -> dict[str, Any]:
        """Save the latest camera frame as a PNG and return its absolute path."""
        return bridge.call("snapshot")

    @server.tool()
    def set_recording(enabled: bool) -> dict[str, Any]:
        """Start or stop video recording. The camera must already be running."""
        return bridge.call("recording", enabled)

    @server.tool()
    def set_camera_settings(
        exposure_ms: float | None = None,
        gain: int | None = None,
        auto_exposure: bool | None = None,
    ) -> dict[str, Any]:
        """Update supplied camera settings; omit settings that should remain unchanged."""
        if exposure_ms is None and gain is None and auto_exposure is None:
            return {"ok": False, "message": "Provide at least one setting."}
        results: dict[str, Any] = {}
        if auto_exposure is not None:
            results["auto_exposure"] = bridge.call("auto_exposure", auto_exposure)
        if exposure_ms is not None:
            results["exposure"] = bridge.call("exposure", exposure_ms)
        if gain is not None:
            results["gain"] = bridge.call("gain", gain)
        return {"ok": all(result["ok"] for result in results.values()), "results": results}

    @server.tool()
    def set_object_detection(enabled: bool) -> dict[str, Any]:
        """Enable or disable the microscope's YOLO object-detection overlay."""
        return bridge.call("yolo", enabled)

    @server.tool()
    def move_stage(
        x_mm: float,
        y_mm: float,
        feedrate_mm_min: int = 500,
        confirm: bool = False,
    ) -> dict[str, Any]:
        """Queue an absolute XY stage move after the user explicitly confirms it."""
        if not confirm:
            return {
                "ok": False,
                "message": (
                    "Stage motion requires explicit user confirmation. Explain the target "
                    "and call again with confirm=true only after they confirm."
                ),
            }
        return bridge.call("move_stage", x_mm, y_mm, feedrate_mm_min)

    @server.tool()
    def emergency_stop_stage() -> dict[str, Any]:
        """Immediately request a stage stop. Use when stage motion should be halted."""
        return bridge.call("emergency_stop_stage")

    return server


def start_local_mcp_server(bridge: QtMicroscopeBridge) -> threading.Thread | None:
    """Start the loopback-only Streamable HTTP listener in a daemon thread."""
    if os.environ.get("MICROSCOPE_MCP_ENABLED", "1").lower() in {"0", "false", "no"}:
        LOG.info("Microscope MCP server disabled by MICROSCOPE_MCP_ENABLED.")
        return None

    try:
        port = int(os.environ.get("MICROSCOPE_MCP_PORT", "8765"))
        if not 1 <= port <= 65535:
            raise ValueError
    except ValueError:
        raise ValueError("MICROSCOPE_MCP_PORT must be an integer from 1 through 65535.")

    def run() -> None:
        try:
            create_server(bridge, port=port).run(transport="streamable-http")
        except ModuleNotFoundError:
            LOG.warning("MCP integration is unavailable; install the project's MCP dependency.")
        except Exception:
            LOG.exception("Microscope MCP server stopped unexpectedly.")

    thread = threading.Thread(target=run, name="microscope-mcp", daemon=True)
    thread.start()
    LOG.info("Microscope MCP server starting at http://127.0.0.1:%s/mcp", port)
    return thread
