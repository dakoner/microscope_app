"""Local MCP servers for the microscope hardware daemon and GUI workstation.

Supports three profiles:
- ``hardware``: Camera and stage hardware tools hosted by ``run_camera_server.py``
  (default ``http://127.0.0.1:8765/mcp``).
- ``app``: Workstation/GUI-only tools (YOLO overlay, mosaic/scanning status)
  hosted by ``run_microscope_app.py`` when connected to a remote camera server
  (default ``http://127.0.0.1:8767/mcp``), avoiding duplicate hardware tools.
- ``full``: Combined hardware + GUI tools hosted by ``run_microscope_app.py``
  when running directly against local hardware (default ``http://127.0.0.1:8765/mcp``).

All listeners default to loopback (``127.0.0.1``). Tool calls are marshaled onto
the Qt thread through ``QtMicroscopeBridge``; the MCP worker never touches
native hardware or Qt objects directly.
"""

from __future__ import annotations

import json
import logging
import os
import threading
from dataclasses import dataclass
from typing import Any, Callable, Literal

from PySide6.QtCore import QObject, QThread, Qt, Signal, Slot


LOG = logging.getLogger(__name__)

McpMode = Literal["full", "hardware", "app"]
TargetKind = Literal["window", "camera_server"]

DEFAULT_HARDWARE_MCP_PORT = 8765
DEFAULT_APP_MCP_PORT = 8767


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

    def __init__(
        self,
        native_module: Any,
        target_ptr: int,
        *,
        target_kind: TargetKind = "window",
    ) -> None:
        super().__init__()
        if target_kind not in {"window", "camera_server"}:
            raise ValueError(f"Unsupported MCP target_kind: {target_kind}")
        self._native = native_module
        self._target_ptr = target_ptr
        self._target_kind: TargetKind = target_kind
        self._closed = False
        self._requested.connect(self._execute, Qt.QueuedConnection)

    @property
    def target_kind(self) -> TargetKind:
        return self._target_kind

    def close(self) -> None:
        self._closed = True

    def call(self, operation: str, *args: Any, timeout_seconds: float = 15) -> dict[str, Any]:
        if self._closed:
            raise RuntimeError("The microscope server is closing.")

        # Useful for direct tests and avoids a deadlock if a call ever originates
        # from the Qt thread.
        if QThread.currentThread() == self.thread():
            return self._call_native(operation, args)

        pending = _PendingCall(operation, args, threading.Event())
        self._requested.emit(pending)
        if not pending.done.wait(timeout_seconds):
            raise TimeoutError("The microscope server did not answer the MCP request in time.")
        if pending.error:
            raise pending.error
        if pending.result is None:
            raise RuntimeError("The microscope server returned no result.")
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
        if self._target_kind == "camera_server":
            methods: dict[str, Callable[..., str]] = {
                "status": lambda: self._native.camera_server_mcp_status_ptr(self._target_ptr),
                "start_camera": lambda: self._native.camera_server_mcp_start_camera_ptr(self._target_ptr),
                "stop_camera": lambda: self._native.camera_server_mcp_stop_camera_ptr(self._target_ptr),
                "snapshot": lambda: self._native.camera_server_mcp_snapshot_ptr(self._target_ptr),
                "recording": lambda: self._native.camera_server_mcp_set_recording_ptr(self._target_ptr, *args),
                "exposure": lambda: self._native.camera_server_mcp_set_exposure_ptr(self._target_ptr, *args),
                "gain": lambda: self._native.camera_server_mcp_set_gain_ptr(self._target_ptr, *args),
                "auto_exposure": lambda: self._native.camera_server_mcp_set_auto_exposure_ptr(
                    self._target_ptr, *args
                ),
                "move_stage": lambda: self._native.camera_server_mcp_move_stage_ptr(self._target_ptr, *args),
                "emergency_stop_stage": lambda: self._native.camera_server_mcp_emergency_stop_stage_ptr(
                    self._target_ptr
                ),
            }
        else:
            methods = {
                "status": lambda: self._native.mcp_status_ptr(self._target_ptr),
                "start_camera": lambda: self._native.mcp_start_camera_ptr(self._target_ptr),
                "stop_camera": lambda: self._native.mcp_stop_camera_ptr(self._target_ptr),
                "snapshot": lambda: self._native.mcp_snapshot_ptr(self._target_ptr),
                "recording": lambda: self._native.mcp_set_recording_ptr(self._target_ptr, *args),
                "exposure": lambda: self._native.mcp_set_exposure_ptr(self._target_ptr, *args),
                "gain": lambda: self._native.mcp_set_gain_ptr(self._target_ptr, *args),
                "auto_exposure": lambda: self._native.mcp_set_auto_exposure_ptr(self._target_ptr, *args),
                "yolo": lambda: self._native.mcp_set_yolo_ptr(self._target_ptr, *args),
                "move_stage": lambda: self._native.mcp_move_stage_ptr(self._target_ptr, *args),
                "emergency_stop_stage": lambda: self._native.mcp_emergency_stop_stage_ptr(self._target_ptr),
                "start_scan": lambda: self._native.mcp_start_scan_ptr(self._target_ptr, *args),
                "cancel_scan": lambda: self._native.mcp_cancel_scan_ptr(self._target_ptr),
                "start_autofocus": lambda: self._native.mcp_start_autofocus_ptr(self._target_ptr, *args),
                "cancel_autofocus": lambda: self._native.mcp_cancel_autofocus_ptr(self._target_ptr),
            }
        try:
            response = methods[operation]()
        except KeyError as exc:
            raise ValueError(f"Unsupported microscope MCP operation: {operation}") from exc
        return json.loads(response)


def _register_hardware_tools(server: Any, bridge: QtMicroscopeBridge) -> None:
    @server.tool()
    def get_microscope_status() -> dict[str, Any]:
        """Get camera, recording, and stage position/state."""
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


def _register_app_tools(server: Any, bridge: QtMicroscopeBridge, *, include_app_status: bool) -> None:
    if include_app_status:
        @server.tool()
        def get_app_status() -> dict[str, Any]:
            """Get workstation GUI status (remote endpoint, YOLO overlay, scanning, mosaic/frame counts)."""
            raw = bridge.call("status")
            app_fields = (
                "ok",
                "message",
                "camera_mode",
                "remote_camera_host",
                "remote_camera_port",
                "remote_jpeg_quality",
                "camera_running",
                "camera_connected",
                "recording",
                "remote_recording",
                "scanning",
                "yolo_available",
                "yolo_inference",
                "frames_received",
                "frames_written_to_mosaic",
                "fps",
                "stage_connected",
                "stage_port",
                "cnc_state",
                "stage_position_mm",
                "focus_score",
                "autofocus_active",
            )
            result = {k: raw[k] for k in app_fields if k in raw}
            result["role"] = "app"
            return result

    @server.tool()
    def set_object_detection(enabled: bool) -> dict[str, Any]:
        """Enable or disable the microscope GUI's YOLO object-detection overlay."""
        return bridge.call("yolo", enabled)

    if bridge.target_kind == "window":

        @server.tool()
        def start_scan(
            x_min: float,
            y_min: float,
            x_max: float,
            y_max: float,
            feedrate_mm_min: int | None = None,
            serpentine: bool | None = None,
            home_x: bool | None = None,
            home_y: bool | None = None,
            confirm: bool = False,
        ) -> dict[str, Any]:
            """Start a rectangular mosaic scan over stage coordinates after confirmation.

            Coordinates are millimetres and describe the region corners, not tile
            counts. Omitted feedrate/path/homing arguments keep the values the
            Stage Control panel currently shows.
            """
            if not confirm:
                return {
                    "ok": False,
                    "message": (
                        "A scan drives the stage continuously across the region. Explain "
                        "the region and call again with confirm=true only after the user "
                        "confirms it."
                    ),
                }
            return bridge.call(
                "start_scan",
                x_min,
                y_min,
                x_max,
                y_max,
                feedrate_mm_min,
                serpentine,
                home_x,
                home_y,
            )

        @server.tool()
        def cancel_scan() -> dict[str, Any]:
            """Cancel the running mosaic scan; the stage is feed-held and stopped."""
            return bridge.call("cancel_scan")

        @server.tool()
        def autofocus(
            range_mm: float = 0.0,
            step_mm: float = 0.0,
            confirm: bool = False,
        ) -> dict[str, Any]:
            """Drive the Z stage through a focus sweep and stop at the sharpest point.

            The stage walks the search range in coarse steps, then refines around the
            best sharpness found, then returns to it.  Auto exposure is paused for the
            duration so the score stays comparable.  Pass confirm=True to allow motion.

            range_mm: total Z travel to search, centred on the current Z. 0 keeps the
                value the Stage Control panel shows.
            step_mm: coarse step in millimetres; the refinement pass uses a quarter of
                it.  0 keeps the panel's value.
            """
            if not confirm:
                return {
                    "success": False,
                    "message": (
                        "autofocus moves the stage. Re-call with confirm=True to allow motion."
                    ),
                }
            return bridge.call("start_autofocus", float(range_mm), float(step_mm))

        @server.tool()
        def cancel_autofocus() -> dict[str, Any]:
            """Stop a running autofocus search where the stage currently is."""
            return bridge.call("cancel_autofocus")


def create_server(
    bridge: QtMicroscopeBridge,
    *,
    port: int,
    host: str = "127.0.0.1",
    mode: McpMode = "full",
) -> Any:
    """Create an MCP server without starting a listener.

    Importing MCP here keeps the regular microscope tools usable when optional
    MCP dependencies have not been installed yet.
    """
    if mode not in {"full", "hardware", "app"}:
        raise ValueError(f"Unsupported MCP mode: {mode}")

    from mcp.server.fastmcp import FastMCP

    if mode == "hardware":
        name = "microscope-hardware"
        instructions = (
            "Control the microscope camera and CNC stage hardware server. Inspect status "
            "before changing hardware. Never move the stage unless the user explicitly "
            "confirms the destination and the move_stage tool's confirm argument is true."
        )
    elif mode == "app":
        name = "microscope-app"
        instructions = (
            "Control workstation GUI features of the microscope application (such as YOLO "
            "object-detection overlay, mosaic scanning, and scanning status). Direct camera "
            "and stage hardware control is provided by the separate microscope-hardware MCP "
            "server. Never start a scan unless the user explicitly confirms the region and "
            "the start_scan tool's confirm argument is true."
        )
    else:
        name = "microscope-app"
        instructions = (
            "Control a local microscope application. Inspect status before changing "
            "hardware. Never move the stage unless the user explicitly confirms the "
            "destination and the move_stage tool's confirm argument is true."
        )

    server = FastMCP(
        name,
        instructions=instructions,
        # FastMCP 1.x receives listener configuration at construction time;
        # run() only receives the transport selection.
        host=host,
        port=port,
        streamable_http_path="/mcp",
    )

    if mode in {"full", "hardware"}:
        _register_hardware_tools(server, bridge)
    if mode in {"full", "app"}:
        _register_app_tools(server, bridge, include_app_status=(mode == "app"))

    return server


def start_local_mcp_server(
    bridge: QtMicroscopeBridge,
    *,
    mode: McpMode = "full",
    port: int | None = None,
    host: str | None = None,
    enabled: bool | None = None,
) -> threading.Thread | None:
    """Start the Streamable HTTP MCP listener in a daemon thread."""
    if enabled is False:
        LOG.info("Microscope MCP server (%s) disabled by CLI flag.", mode)
        return None
    if enabled is None and os.environ.get("MICROSCOPE_MCP_ENABLED", "1").lower() in {
        "0",
        "false",
        "no",
    }:
        LOG.info("Microscope MCP server (%s) disabled by MICROSCOPE_MCP_ENABLED.", mode)
        return None

    bind_host = (host or os.environ.get("MICROSCOPE_MCP_BIND") or "127.0.0.1").strip()
    if not bind_host:
        bind_host = "127.0.0.1"

    default_port = DEFAULT_APP_MCP_PORT if mode == "app" else DEFAULT_HARDWARE_MCP_PORT
    if port is None:
        env_port = os.environ.get("MICROSCOPE_MCP_PORT")
        try:
            resolved_port = int(env_port) if env_port else default_port
        except ValueError as exc:
            raise ValueError("MICROSCOPE_MCP_PORT must be an integer from 1 through 65535.") from exc
    else:
        resolved_port = port

    if not 1 <= resolved_port <= 65535:
        raise ValueError("MCP port must be an integer from 1 through 65535.")

    def run() -> None:
        try:
            create_server(bridge, port=resolved_port, host=bind_host, mode=mode).run(
                transport="streamable-http"
            )
        except ModuleNotFoundError:
            LOG.warning("MCP integration is unavailable; install the project's MCP dependency.")
        except Exception:
            LOG.exception("Microscope MCP server (%s) stopped unexpectedly.", mode)

    thread = threading.Thread(target=run, name=f"microscope-mcp-{mode}", daemon=True)
    thread.start()
    print(f"Microscope MCP server ({mode}) starting at http://{bind_host}:{resolved_port}/mcp", flush=True)
    LOG.info("Microscope MCP server (%s) starting at http://%s:%s/mcp", mode, bind_host, resolved_port)
    return thread
