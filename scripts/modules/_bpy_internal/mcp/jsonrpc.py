# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Transport-agnostic JSON-RPC 2.0 message framing and request/response
matching, used by the MCP (Model Context Protocol) client.

This module only depends on the Python standard library and MUST remain
importable -- and unit-testable -- without ``bpy`` (see
``tests/python/mcp_jsonrpc_test.py``).

All JSON coming from external, untrusted MCP servers is parsed here using
the stdlib :mod:`json` module only. There is intentionally no ``eval()``
and no ``pickle`` anywhere in this file: those would let a malicious or
compromised MCP server execute arbitrary code merely by sending bytes.
Messages are also capped at :data:`MAX_MESSAGE_SIZE` to bound how much
untrusted input the parser is ever asked to hold in memory at once.
"""

from __future__ import annotations

__all__ = (
    "JSONRPC_VERSION",
    "MAX_MESSAGE_SIZE",
    "ProtocolError",
    "RemoteError",
    "IdAllocator",
    "encode_message",
    "decode_message",
    "make_request",
    "make_notification",
    "Session",
)

import itertools
import json
import threading
from typing import Any

JSONRPC_VERSION = "2.0"

# Hard cap on the size of a single JSON-RPC message, in either direction.
# This bounds how much attacker-controlled input `json.loads` is ever
# asked to parse in one go.
MAX_MESSAGE_SIZE = 16 * 1024 * 1024  # 16 MiB


class ProtocolError(Exception):
    """A JSON-RPC message was malformed or violated framing rules.

    Raised for things like: not valid JSON, not a JSON object, missing
    the ``jsonrpc`` version, or exceeding :data:`MAX_MESSAGE_SIZE`. Never
    raised because a peer's *content* was semantically an error -- see
    :class:`RemoteError` for that.
    """


class RemoteError(Exception):
    """The JSON-RPC peer replied with an ``error`` response object."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        super().__init__(f"JSON-RPC error {code}: {message}")
        self.code = code
        self.message = message
        self.data = data


class IdAllocator:
    """Thread-safe generator of incrementing integer JSON-RPC request ids."""

    def __init__(self, start: int = 1) -> None:
        self._lock = threading.Lock()
        self._counter = itertools.count(start)

    def next_id(self) -> int:
        with self._lock:
            return next(self._counter)


def _byte_length(data: bytes | bytearray | str) -> int:
    if isinstance(data, (bytes, bytearray)):
        return len(data)
    return len(data.encode("utf-8"))


def encode_message(obj: dict) -> bytes:
    """Serialize a JSON-RPC message object to UTF-8 JSON bytes.

    Uses :func:`json.dumps` exclusively. Raises :class:`ProtocolError` if
    the encoded message would exceed :data:`MAX_MESSAGE_SIZE`.
    """
    data = json.dumps(obj, ensure_ascii=True).encode("utf-8")
    if len(data) > MAX_MESSAGE_SIZE:
        raise ProtocolError(
            f"Outgoing JSON-RPC message of {len(data)} bytes exceeds the "
            f"{MAX_MESSAGE_SIZE} byte limit"
        )
    return data


def decode_message(data: bytes | bytearray | str) -> dict:
    """Parse a single JSON-RPC message.

    Uses :func:`json.loads` exclusively -- never ``eval``, never
    ``pickle``. Raises :class:`ProtocolError` (never lets a raw
    :class:`json.JSONDecodeError` or other stdlib exception escape) so
    callers can treat a bad message from an external, untrusted server as
    a recoverable protocol error instead of a crash.
    """
    size = _byte_length(data)
    if size > MAX_MESSAGE_SIZE:
        raise ProtocolError(
            f"Incoming JSON-RPC message of {size} bytes exceeds the "
            f"{MAX_MESSAGE_SIZE} byte limit"
        )

    try:
        obj = json.loads(data)
    except (json.JSONDecodeError, TypeError, ValueError, UnicodeDecodeError) as ex:
        raise ProtocolError(f"Malformed JSON-RPC message: {ex}") from ex

    if not isinstance(obj, dict):
        raise ProtocolError("JSON-RPC message must be a JSON object")
    if obj.get("jsonrpc") != JSONRPC_VERSION:
        raise ProtocolError("JSON-RPC message is missing a supported 'jsonrpc' version")

    return obj


def make_request(method: str, params: dict | list | None = None, id_: int | None = None) -> dict:
    """Build a JSON-RPC 2.0 request object."""
    request: dict = {"jsonrpc": JSONRPC_VERSION, "id": id_, "method": method}
    if params is not None:
        request["params"] = params
    return request


def make_notification(method: str, params: dict | list | None = None) -> dict:
    """Build a JSON-RPC 2.0 notification object (no ``id``, no reply expected)."""
    notification: dict = {"jsonrpc": JSONRPC_VERSION, "method": method}
    if params is not None:
        notification["params"] = params
    return notification


class Session:
    """Transport-agnostic JSON-RPC request/response id matching.

    A :class:`Session` knows nothing about pipes, sockets or HTTP: it is
    handed a ``send`` callable (``bytes -> None``) used to dispatch
    outgoing messages, and its owner feeds it raw incoming messages via
    :meth:`handle_message`. This keeps it trivially unit-testable in
    isolation, and reusable across the stdio and HTTP transports.
    """

    def __init__(self, send: Any) -> None:
        self._send = send
        self._ids = IdAllocator()
        self._lock = threading.Lock()
        # request id -> {"event": threading.Event, "result": Any, "error": RemoteError | None}
        self._pending: dict[int, dict[str, Any]] = {}
        # Notifications, and requests initiated by the server, queued up
        # for a caller (e.g. `pump()`) to drain. Not dispatched here:
        # v1 has no server-initiated request handlers.
        self.incoming_notifications: list[dict] = []

    def send_request(self, method: str, params: dict | list | None = None) -> int:
        """Encode and send a request, returning its id for later matching."""
        id_ = self._ids.next_id()
        request = make_request(method, params, id_)
        with self._lock:
            self._pending[id_] = {"event": threading.Event(), "result": None, "error": None}
        self._send(encode_message(request))
        return id_

    def send_notification(self, method: str, params: dict | list | None = None) -> None:
        """Encode and send a notification (no response is expected)."""
        self._send(encode_message(make_notification(method, params)))

    def handle_message(self, raw: bytes | bytearray | str) -> None:
        """Decode and route one incoming message.

        Responses are matched to a pending request by id, and unblock any
        caller waiting in :meth:`wait_response`. Anything else (a
        notification, or a request initiated by the server) is appended to
        :attr:`incoming_notifications` instead of being dropped silently.
        """
        message = decode_message(raw)

        if "id" in message and ("result" in message or "error" in message):
            self._resolve(message)
            return

        if "method" in message:
            # Either a notification (no `id`) or a server-initiated
            # request (`method` + `id`). v1 does not act on either -- they
            # are only queued so `pump()` can observe/log them.
            with self._lock:
                self.incoming_notifications.append(message)
            return

        raise ProtocolError(f"Unroutable JSON-RPC message: {message!r}")

    def _resolve(self, message: dict) -> None:
        id_ = message.get("id")
        with self._lock:
            pending = self._pending.get(id_)
        if pending is None:
            # Unknown id: stale, duplicate, or a response to a request we
            # never made (or already timed out on). An unsolicited or
            # duplicate message from a server must never crash the client.
            return

        error = message.get("error")
        if error is not None:
            if isinstance(error, dict):
                pending["error"] = RemoteError(
                    error.get("code", -1),
                    error.get("message", "Unknown error"),
                    error.get("data"),
                )
            else:
                pending["error"] = RemoteError(-1, str(error))
        else:
            pending["result"] = message.get("result")
        pending["event"].set()

    def wait_response(self, id_: int, timeout: float | None = None) -> Any:
        """Block until the response to ``id_`` arrives, or ``timeout`` elapses.

        Raises :class:`RemoteError` if the peer replied with an error,
        :class:`TimeoutError` if ``timeout`` elapses first, and otherwise
        returns the response's ``result`` value.
        """
        with self._lock:
            pending = self._pending.get(id_)
        if pending is None:
            raise KeyError(f"No pending request with id {id_!r}")

        if not pending["event"].wait(timeout):
            raise TimeoutError(f"Timed out waiting for a response to request {id_!r}")

        with self._lock:
            self._pending.pop(id_, None)

        if pending["error"] is not None:
            raise pending["error"]
        return pending["result"]

    def cancel(self, id_: int) -> None:
        """Drop a pending request without waiting for its response."""
        with self._lock:
            self._pending.pop(id_, None)

    def pending_count(self) -> int:
        with self._lock:
            return len(self._pending)

    def drain_notifications(self) -> list[dict]:
        """Atomically pop and return all queued notifications/requests."""
        with self._lock:
            notifications = self.incoming_notifications
            self.incoming_notifications = []
        return notifications
