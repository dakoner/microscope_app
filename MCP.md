# Microscope MCP integration

Launching `microscope-app` also starts a local, Streamable HTTP MCP server at
`http://127.0.0.1:8765/mcp`. It is bound to loopback only, so it is not exposed
to the local network.

Install the project dependencies first (the MCP SDK is included in
`pyproject.toml`). Then add the URL above as a Streamable HTTP MCP connector in
your AI client. The server provides status, camera, snapshot, recording,
camera-settings, object-detection, stage-motion, and emergency-stop tools.

Stage motion requires `confirm=true`; an AI should set that only after the user
has explicitly confirmed the requested destination. No unrestricted G-code
execution is exposed.

Set `MICROSCOPE_MCP_PORT` to choose another local port, or set
`MICROSCOPE_MCP_ENABLED=0` to launch the UI without the MCP server.
