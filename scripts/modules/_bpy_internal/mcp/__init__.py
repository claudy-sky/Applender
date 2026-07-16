# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
MCP (Model Context Protocol) client.

This package is the "substance" of Applender's native MCP support: it owns
JSON-RPC 2.0 transport (stdio subprocess and HTTP, see
:mod:`_bpy_internal.mcp.transport`) and all JSON parsing (see
:mod:`_bpy_internal.mcp.jsonrpc`).

Security rationale: JSON received from external, untrusted MCP servers is
parsed exclusively by Python's memory-safe standard library ``json`` module
here -- it is never hand-parsed by C++. The native facade in
``intern/mcp`` (``blender::mcp``) and the ``bpy.mcp`` Python submodule are
both thin wrappers that call into this module; neither of them touches raw
server bytes directly.

This module additionally imports ``bpy`` (guarded, see
:func:`default_config_path`), so unlike :mod:`_bpy_internal.mcp.jsonrpc`
and :mod:`_bpy_internal.mcp.transport` it is not usable from a fully
bpy-free interpreter -- it degrades gracefully instead (falling back to an
environment variable / the home directory) so it stays importable for
tooling such as `python3 -m py_compile`.
"""

from __future__ import annotations

__all__ = (
    "configure_from_file",
    "configure",
    "default_config_path",
    "connect",
    "disconnect",
    "is_connected",
    "list_servers",
    "list_tools",
    "call_tool",
    "list_resources",
    "read_resource",
    "pump",
)

import json
import os
import threading
from typing import Any

from . import jsonrpc
from . import transport as _transport

# MCP protocol version this client speaks. Kept as a plain string constant
# (not negotiated) for v1.
MCP_PROTOCOL_VERSION = "2024-11-05"

CLIENT_NAME = "Applender"
CLIENT_VERSION = "1.0.0"

CONFIG_FILENAME = "mcp_servers.json"

# How long `connect()` waits for its worker thread to notice its own
# timeout and give up, on top of the caller-provided timeout. This only
# guards against a worker thread that is somehow stuck even past its own
# `Session.wait_response` timeout.
_CONNECT_JOIN_SLACK = 1.0


class _ServerConnection:
    """Everything the client tracks for one connected MCP server."""

    __slots__ = ("name", "config", "transport", "session", "initialized", "lock", "server_info")

    def __init__(self, name: str, config: dict) -> None:
        self.name = name
        self.config = config
        self.transport: _transport.Transport | None = None
        self.session: jsonrpc.Session | None = None
        self.initialized = False
        self.lock = threading.Lock()
        self.server_info: Any = None


# Server registry: name -> config dict, as passed to `configure()`.
_servers: dict[str, dict] = {}
# Live connections: name -> _ServerConnection, only present once `connect()`
# has completed the MCP `initialize` handshake successfully.
_connections: dict[str, _ServerConnection] = {}
_registry_lock = threading.Lock()


def default_config_path() -> str:
    """Return the path to the user's MCP server config file.

    Uses `bpy.utils.user_resource('CONFIG')` when `bpy` is importable
    (the normal case, running inside Blender). Falls back to the
    `BLENDER_USER_CONFIG` environment variable (the same override Blender's
    own C++ `appdir` resolution honors), and finally to a directory under
    the user's home, so this function -- and therefore this module -- stays
    importable from a plain `python3` interpreter (tests, `py_compile`).
    """
    config_dir: str | None = None
    try:
        import bpy
        config_dir = bpy.utils.user_resource('CONFIG')
    except Exception:
        config_dir = None

    if not config_dir:
        config_dir = os.environ.get("BLENDER_USER_CONFIG")
    if not config_dir:
        config_dir = os.path.join(os.path.expanduser("~"), ".config", "blender")

    return os.path.join(config_dir, CONFIG_FILENAME)


def configure(servers: dict) -> None:
    """Replace the server registry from an in-memory dict.

    ``servers`` maps a server name to its config, e.g.::

        {
            "my-stdio-server": {
                "transport": "stdio",
                "command": ["my-mcp-server", "--flag"],
                "env": {"SOME_VAR": "value"},
            },
            "my-http-server": {
                "transport": "http",
                "url": "https://example.org/mcp",
            },
        }
    """
    if not isinstance(servers, dict):
        raise TypeError("servers must be a dict of {name: config}")
    with _registry_lock:
        _servers.clear()
        _servers.update(servers)


def configure_from_file(path: str | None = None) -> None:
    """Load the server registry from a JSON config file.

    ``path`` defaults to :func:`default_config_path`. A missing file is
    treated as an empty registry (not an error), so a fresh install with no
    configured MCP servers works out of the box.
    """
    if path is None:
        path = default_config_path()

    if not os.path.exists(path):
        configure({})
        return

    with open(path, "r", encoding="utf-8") as fh:
        raw = fh.read()

    try:
        data = json.loads(raw)
    except json.JSONDecodeError as ex:
        raise jsonrpc.ProtocolError(f"Malformed MCP server config at {path!r}: {ex}") from ex

    if isinstance(data, dict) and isinstance(data.get("servers"), dict):
        servers = data["servers"]
    elif isinstance(data, dict):
        servers = data
    else:
        servers = {}

    configure(servers)


def list_servers() -> list[str]:
    """Return the names of every configured (not necessarily connected) server."""
    with _registry_lock:
        return sorted(_servers.keys())


def is_connected(name: str) -> bool:
    with _registry_lock:
        conn = _connections.get(name)
    return bool(conn is not None and conn.initialized)


def _get_config(name: str) -> dict:
    with _registry_lock:
        config = _servers.get(name)
    if config is None:
        raise KeyError(f"No MCP server named {name!r} is configured")
    return config


def _make_transport(config: dict) -> _transport.Transport:
    kind = config.get("transport")
    if kind == "stdio":
        command = config.get("command")
        if not command:
            raise ValueError("stdio MCP server config requires a non-empty 'command' list")
        return _transport.StdioTransport(command, env=config.get("env"))
    if kind == "http":
        url = config.get("url")
        if not url:
            raise ValueError("http MCP server config requires a 'url'")
        return _transport.HttpTransport(url, headers=config.get("headers"))
    raise ValueError(f"Unknown MCP server transport {kind!r}, expected 'stdio' or 'http'")


def _reader_loop(conn: _ServerConnection) -> None:
    """Background thread body: pump raw messages from the transport into
    the JSON-RPC session for id-matching, until the transport goes away."""
    while True:
        transport = conn.transport
        if transport is None:
            return
        try:
            raw = transport.recv(timeout=1.0)
        except Exception:
            return

        if raw is None:
            if not getattr(transport, "is_alive", True):
                return
            continue

        session = conn.session
        if session is None:
            continue
        try:
            session.handle_message(raw)
        except jsonrpc.ProtocolError:
            # A malformed message from the server is a recoverable
            # protocol error, not a reason to kill the reader thread.
            continue


def connect(name: str, timeout: float = 30.0) -> bool:
    """Start the transport for server ``name`` and perform the MCP
    ``initialize`` handshake.

    The (blocking) transport startup and handshake run on a worker thread;
    this function waits on a `threading.Event` bounded by ``timeout``, so
    callers such as a modal operator get a bounded call regardless of how
    slow or unresponsive the server is. Returns ``True`` on success,
    ``False`` on any failure or timeout -- this never raises for ordinary
    connection failures.
    """
    config = _get_config(name)

    conn = _ServerConnection(name, config)
    done = threading.Event()
    outcome = {"ok": False}

    def worker() -> None:
        try:
            t = _make_transport(config)
            conn.transport = t
            session = jsonrpc.Session(send=t.send)
            conn.session = session

            reader = threading.Thread(
                target=_reader_loop, args=(conn,), name=f"mcp-reader-{name}", daemon=True
            )
            reader.start()

            request_id = session.send_request(
                "initialize",
                {
                    "protocolVersion": MCP_PROTOCOL_VERSION,
                    "capabilities": {},
                    "clientInfo": {"name": CLIENT_NAME, "version": CLIENT_VERSION},
                },
            )
            conn.server_info = session.wait_response(request_id, timeout=timeout)
            session.send_notification("notifications/initialized")
            conn.initialized = True
            outcome["ok"] = True
        except Exception:
            try:
                if conn.transport is not None:
                    conn.transport.close()
            except Exception:
                pass
        finally:
            done.set()

    worker_thread = threading.Thread(target=worker, name=f"mcp-connect-{name}", daemon=True)
    worker_thread.start()

    if not done.wait(timeout + _CONNECT_JOIN_SLACK):
        # The worker thread is stuck past its own bounded wait (e.g. a
        # hung subprocess start). Don't block the caller forever.
        return False

    if outcome["ok"]:
        with _registry_lock:
            _connections[name] = conn
        return True
    return False


def disconnect(name: str) -> None:
    with _registry_lock:
        conn = _connections.pop(name, None)
    if conn is None:
        return
    conn.initialized = False
    try:
        if conn.transport is not None:
            conn.transport.close()
    except Exception:
        pass


def _get_connection(name: str) -> _ServerConnection:
    with _registry_lock:
        conn = _connections.get(name)
    if conn is None or not conn.initialized or conn.session is None:
        raise RuntimeError(f"MCP server {name!r} is not connected; call connect() first")
    return conn


def _request(name: str, method: str, params: dict | None, timeout: float) -> Any:
    conn = _get_connection(name)
    request_id = conn.session.send_request(method, params)
    return conn.session.wait_response(request_id, timeout=timeout)


def list_tools(name: str, timeout: float = 30.0) -> list[dict]:
    """Return the `tools/list` result's `tools` array for server ``name``."""
    result = _request(name, "tools/list", {}, timeout)
    if isinstance(result, dict):
        tools = result.get("tools", [])
        if isinstance(tools, list):
            return tools
    return []


def call_tool(name: str, tool: str, arguments: dict, timeout: float = 60.0) -> dict:
    """Call `tools/call` on server ``name`` for tool ``tool`` with ``arguments``."""
    result = _request(
        name, "tools/call", {"name": tool, "arguments": arguments or {}}, timeout
    )
    return result if isinstance(result, dict) else {"result": result}


def list_resources(name: str, timeout: float = 30.0) -> list[dict]:
    """Return the `resources/list` result's `resources` array for server ``name``."""
    result = _request(name, "resources/list", {}, timeout)
    if isinstance(result, dict):
        resources = result.get("resources", [])
        if isinstance(resources, list):
            return resources
    return []


def read_resource(name: str, uri: str, timeout: float = 30.0) -> dict:
    """Call `resources/read` on server ``name`` for the given resource ``uri``."""
    result = _request(name, "resources/read", {"uri": uri}, timeout)
    return result if isinstance(result, dict) else {"result": result}


def pump() -> float:
    """Drain queued notifications for every connected server.

    Non-blocking. Intended to be driven by `bpy.app.timers.register()` on
    the UI thread so background I/O (arriving on the per-server reader
    threads) never blocks the interface. Returns the interval, in seconds,
    the caller should wait before calling `pump()` again -- matching the
    `bpy.app.timers` convention of a registered function returning its own
    next interval.
    """
    with _registry_lock:
        connections = list(_connections.values())

    for conn in connections:
        if conn.session is None:
            continue
        # v1 has no notification handlers wired up yet; draining here is
        # enough to bound the queue's memory use. This is the hook future
        # native UI/operators can use to react to server-sent notifications.
        conn.session.drain_notifications()

    return 0.5
