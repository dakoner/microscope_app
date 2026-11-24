# Microscope MCP integration

The microscope stack splits MCP responsibilities between the **hardware server**
and the **GUI workstation**, while keeping a combined server when the GUI runs
directly against local hardware.

All MCP servers use Streamable HTTP and bind to `127.0.0.1` (loopback only) by
default so unauthenticated hardware controls are never exposed to the LAN unless
explicitly configured.

## Server profiles

### 1. Headless hardware server (`microscope-camera-server` / `run_camera_server.py`)
- **Default URL:** `http://127.0.0.1:8765/mcp`
- **Server name:** `microscope-hardware`
- **Tools:**
  - `get_microscope_status` — camera, recording, TCP client count, and stage state/position
  - `start_camera` / `stop_camera`
  - `capture_snapshot`
  - `set_recording`
  - `set_camera_settings` (`exposure_ms`, `gain`, `auto_exposure`)
  - `move_stage` (requires `confirm=true`)
  - `emergency_stop_stage`

### 2. GUI app in remote mode (`microscope-app --remote HOST[:PORT]`)
When the GUI connects to a remote `camera_server`, it automatically switches its
MCP server to **app-only mode** so hardware tools are not duplicated and ports do
not collide:
- **Default URL:** `http://127.0.0.1:8767/mcp`
- **Server name:** `microscope-app`
- **Tools:**
  - `get_app_status` — remote endpoint info, scanning state, YOLO state, FPS, and frame/mosaic counters
  - `set_object_detection` — enable/disable the YOLO overlay
  - `start_scan` (requires `confirm=true`) — run the GUI's mosaic scan over a rectangle
  - `cancel_scan` — cancel that scan
  - `autofocus` (requires `confirm=true`) / `cancel_autofocus` — closed-loop Z autofocus
  - `list_waypoints` / `save_waypoint` / `goto_waypoint` (requires `confirm=true`) / `delete_waypoint` — manage saved `(X, Y, Z)` specimen bookmarks

### 3. GUI app in local mode (`microscope-app`)
When the GUI drives local hardware directly (no `camera_server`), it starts in
**full mode**:
- **Default URL:** `http://127.0.0.1:8765/mcp`
- **Server name:** `microscope-app`
- **Tools:** All hardware tools above plus `set_object_detection`, `start_scan` and
  `cancel_scan`.

## Mosaic scans from MCP

`start_scan(x_min, y_min, x_max, y_max)` starts the same column scan the Stage
Control panel starts, using stage millimetres for the region corners rather than a
mosaic drag:

- The region is highlighted in the mosaic view and shown in the Stage Control
  panel, so the GUI stays in sync and its **Cancel Scan** button works.
- `feedrate_mm_min`, `serpentine`, `home_x` and `home_y` are optional; omitted
  values keep whatever the panel currently shows (defaults: 100 mm/min, serpentine,
  home X). Tile pitch is unchanged: 75 % of the field of view.
- It is refused while another scan runs, or when the stage link cannot be
  established (remote mode connects it on demand, like `move_stage`), the ruler is
  not calibrated, no camera frame is available, or the region leaves the
  configured stage area.
- `cancel_scan` is `cancelScan()`: feed hold plus stop, composite buffer reset, and
  the scan video closed.

## Safety

Stage motion requires `confirm=true`; an AI should set that only after the user
has explicitly confirmed the requested destination. `start_scan` uses the same
rule for the region it will sweep. No unrestricted G-code execution is exposed.

Note that `stop_camera` stops capture for the whole server: unlike the TCP `stop`
command it does not check whether other clients are still streaming and it ignores
`--keep-open`. Treat `move_stage` and `emergency_stop_stage` as off-limits while
the GUI reports `scanning: true`, since MCP moves and the GUI's scan G-code share
one FluidNC link.

## Watching automation happen

Every action taken through the hardware MCP bridge is written to the server
console (`MCP: …`) and broadcast to all connected TCP clients as an
`mcp_action` event:

```json
{ "event": "mcp_action", "kind": "camera", "message": "exposure set to 5.000 ms" }
```

`kind` is one of `camera`, `recording`, `snapshot` or `stage`. The GUI repeats
these lines with a `[MCP]` prefix — camera, recording and snapshot actions in the
main activity log, stage actions in the CNC panel's log window — and re-syncs the
exposure, gain and auto-exposure widgets from the server's `stats` events, so
changes made by an automation client are visible while it works.

`get_microscope_status` reports `connected_clients`, which counts TCP clients
only: MCP calls run in-process on the server, so an MCP client never appears
there and never receives frames. `get_app_status` reports `camera_running` and
`camera_connected` from the state the server last reported rather than from the
GUI's own start/stop flag, so a camera started or stopped by automation (or a
dropped link) shows up there.

## Configuration

CLI flags (supported by both `run_camera_server.py` and `run_microscope_app.py`):
- `--mcp-port PORT` — override the MCP HTTP port
- `--mcp-bind ADDR` — override the bind address (defaults to `127.0.0.1`)
- `--no-mcp` — disable the MCP server
- `--mcp-mode {auto,full,app}` — (`run_microscope_app.py` only; default `auto`)

Environment variables:
- `MICROSCOPE_MCP_ENABLED=0` — disable the MCP server
- `MICROSCOPE_MCP_PORT` — override the default port (`8765` for hardware/full, `8767` for app)
- `MICROSCOPE_MCP_BIND` — override the bind address (default `127.0.0.1`)
- `MICROSCOPE_MCP_MODE` — (`run_microscope_app.py` only: `auto`, `full`, or `app`)
