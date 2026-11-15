# MindVision QObject

This project builds four qmake targets:

1. `mindvision_qobject`: shared Qt/C++ camera and network server/client library (`MindVisionCamera`, `MindVisionCameraServer`, `VideoThread`)
2. `camera_gui`: native Qt Widgets camera application
3. `camera_server`: headless TCP camera server (`QCoreApplication`, no X11/Wayland display needed)
4. `_mindvision_qobject_py`: Python extension module

## Remote / Headless Operation

### Running the Headless Camera Server

From the repository root (Python launcher):
```bash
uv run run_camera_server.py --bind 0.0.0.0 --port 8766
```
Or using the native C++ binary:
```bash
./src/mindvision_qobject/release/camera_server --bind 0.0.0.0 --port 8766
```
Options:
- `--bind <addr>` / `-b`: Bind address (default `0.0.0.0`)
- `--port <port>` / `-p`: TCP port (default `8766`)
- `--jpeg-quality <0..100>` / `-q`: Default stream quality (`1..100` for JPEG compression, `0` for raw uncompressed RGB888; default `85`)
- `--record-dir <dir>` / `-r`: Directory for server-side recordings (default `videos`)
- `--auto-start`: Open and start camera capture immediately on server startup
- `--keep-open`: Keep the camera running even when all clients disconnect
- `--synthetic`: Use a synthetic test-pattern camera when running without physical MindVision hardware

### Connecting from `microscope_app` or `MindVisionCamera`

- **CLI flag**:
  ```bash
  uv run run_microscope_app.py --remote-camera 192.168.1.50:8766
  ```
- **Environment variables**:
  `MICROSCOPE_CAMERA_MODE=remote`, `MICROSCOPE_CAMERA_HOST=192.168.1.50`, `MICROSCOPE_CAMERA_PORT=8766`, `MICROSCOPE_CAMERA_JPEG_QUALITY=85`
- **GUI**:
  Use the **Camera Source** panel at the top of the **Camera -> Exposure / Gain** tab to switch between `Local (MindVision SDK)` and `Remote (Network TCP)`, configure host/port/quality, and click **Apply / Reconnect Camera**.
- **C++ / Python API**:
  ```python
  cam = MindVisionCamera()
  cam.setRemoteMode(True)
  cam.setRemoteEndpoint("192.168.1.50", 8766)
  cam.setRemoteJpegQuality(85)
  cam.open()
  cam.start()
  ```

## Requirements

- Qt 6 (Core, Gui, Network, Widgets).
- MindVision SDK (MVCAMSDK).

## Output

The build places artifacts in the `release` or `debug` folder:

1. `release/libmindvision_qobject.so`
2. `release/camera_gui`
3. `release/camera_server`
4. `release/_mindvision_qobject_py.so`
