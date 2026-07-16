# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Dependency-free transports for the MCP (Model Context Protocol) client.

Two transports are provided, both implementing the same small interface
(:class:`Transport`): :class:`StdioTransport` (a local subprocess speaking
newline-delimited JSON over stdio) and :class:`HttpTransport` (a single-shot
HTTP POST per request).

Only the standard library is used here (``subprocess``, ``urllib``,
``threading``, ``queue``) -- no ``requests``, no third-party HTTP client.
Message *parsing* is deliberately not done in this module: transports move
opaque bytes, framing and JSON parsing live in :mod:`_bpy_internal.mcp.jsonrpc`.

This module MUST remain importable without ``bpy``.
"""

from __future__ import annotations

__all__ = (
    "MAX_MESSAGE_SIZE",
    "TransportError",
    "Transport",
    "StdioTransport",
    "HttpTransport",
)

import os
import queue
import subprocess
import threading
import urllib.error
import urllib.request

# Kept in sync with `jsonrpc.MAX_MESSAGE_SIZE`. Not imported from there to
# keep this module's only coupling to `jsonrpc` be "moves bytes for it".
MAX_MESSAGE_SIZE = 16 * 1024 * 1024  # 16 MiB


class TransportError(Exception):
    """A transport-level failure: process exited, pipe broken, HTTP error, etc."""


class Transport:
    """Common interface implemented by every MCP transport.

    Deliberately minimal: `send`/`recv` move raw bytes, nothing here knows
    about JSON-RPC framing.
    """

    def send(self, data: bytes) -> None:
        raise NotImplementedError

    def recv(self, timeout: float | None = None) -> bytes | None:
        """Return the next available message, or None on timeout/EOF.

        Must never block indefinitely when `timeout` is given.
        """
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError


class StdioTransport(Transport):
    """Runs an MCP server as a subprocess, speaking newline-delimited JSON
    over its stdin/stdout: exactly one JSON-RPC message per line, per the
    MCP stdio transport specification.

    A background reader thread continuously drains the subprocess' stdout
    into a queue so that `recv()` never blocks the caller thread for longer
    than its `timeout`.
    """

    def __init__(self, command: list[str], env: dict[str, str] | None = None) -> None:
        if not command:
            raise TransportError("StdioTransport requires a non-empty command")

        full_env = None
        if env:
            full_env = dict(os.environ)
            full_env.update(env)

        try:
            self._proc = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=full_env,
                bufsize=0,
            )
        except OSError as ex:
            raise TransportError(f"Failed to start MCP server {command!r}: {ex}") from ex

        self._queue: queue.Queue[bytes | None] = queue.Queue()
        self._reader_thread = threading.Thread(
            target=self._read_loop, name="mcp-stdio-reader", daemon=True
        )
        self._reader_thread.start()

    def _read_loop(self) -> None:
        assert self._proc.stdout is not None
        try:
            for line in self._proc.stdout:
                line = line.strip()
                if not line:
                    continue
                if len(line) > MAX_MESSAGE_SIZE:
                    # Drop oversized lines here rather than queueing them;
                    # `jsonrpc.decode_message` enforces the same cap, this
                    # just avoids holding an oversized buffer in the queue.
                    continue
                self._queue.put(line)
        except (ValueError, OSError):
            pass
        finally:
            # Sentinel: wake up any blocked `recv()` with an EOF signal.
            self._queue.put(None)

    def send(self, data: bytes) -> None:
        if self._proc.stdin is None or self._proc.poll() is not None:
            raise TransportError("MCP server process is not running")
        try:
            self._proc.stdin.write(data)
            if not data.endswith(b"\n"):
                self._proc.stdin.write(b"\n")
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError) as ex:
            raise TransportError(f"Failed writing to MCP server stdin: {ex}") from ex

    def recv(self, timeout: float | None = None) -> bytes | None:
        try:
            return self._queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self) -> None:
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
        except OSError:
            pass
        try:
            self._proc.terminate()
        except OSError:
            pass
        try:
            self._proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            try:
                self._proc.kill()
            except OSError:
                pass

    @property
    def is_alive(self) -> bool:
        return self._proc.poll() is None


class HttpTransport(Transport):
    """Sends one JSON-RPC request per HTTP POST and reads back one response.

    v1 scope note: this only implements plain request/response HTTP (the
    non-streaming half of the MCP "Streamable HTTP" transport). Server-sent
    events / streaming responses are out of scope for v1; adding them would
    mean a dedicated SSE-reading background thread feeding `recv()`
    independently of `send()`. For now `recv()` only ever returns a message
    that a preceding `send()` already queued up.
    """

    def __init__(
        self,
        url: str,
        headers: dict[str, str] | None = None,
        timeout: float = 30.0,
    ) -> None:
        self._url = url
        self._headers = {"Content-Type": "application/json", "Accept": "application/json"}
        if headers:
            self._headers.update(headers)
        self._timeout = timeout
        self._responses: queue.Queue[bytes] = queue.Queue()
        self._closed = False

    def send(self, data: bytes) -> None:
        if self._closed:
            raise TransportError("HTTP transport is closed")
        if len(data) > MAX_MESSAGE_SIZE:
            raise TransportError("Outgoing message exceeds the transport size limit")

        request = urllib.request.Request(
            self._url, data=data, headers=self._headers, method="POST"
        )
        try:
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                body = response.read(MAX_MESSAGE_SIZE + 1)
        except urllib.error.URLError as ex:
            raise TransportError(f"MCP HTTP request to {self._url!r} failed: {ex}") from ex

        if len(body) > MAX_MESSAGE_SIZE:
            raise TransportError("HTTP response exceeds the transport size limit")
        if body:
            self._responses.put(body)

    def recv(self, timeout: float | None = None) -> bytes | None:
        try:
            return self._responses.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self) -> None:
        self._closed = True
