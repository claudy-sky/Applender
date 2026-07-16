# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Unit tests for `_bpy_internal.mcp.jsonrpc`: JSON-RPC 2.0 message framing
and request/response matching for the native MCP (Model Context Protocol)
client.

This test deliberately runs WITHOUT `bpy` -- `jsonrpc.py` is pure standard
library and must stay importable (and testable) entirely outside Blender.
Run directly with a plain Python interpreter:

    python3 tests/python/mcp_jsonrpc_test.py
"""

from __future__ import annotations

__all__ = (
    "main",
)

import json
import sys
import unittest
from pathlib import Path

# `_bpy_internal` lives under `scripts/modules`, which is only added to
# `sys.path` automatically when running inside Blender. Add it explicitly
# here so this test can run under a plain `python3` interpreter -- matching
# the requirement that `_bpy_internal.mcp.jsonrpc` stay bpy-free.
_SCRIPTS_MODULES_DIR = Path(__file__).resolve().parent.parent.parent / "scripts" / "modules"
if str(_SCRIPTS_MODULES_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_MODULES_DIR))

from _bpy_internal.mcp import jsonrpc  # noqa: E402


class IdAllocatorTest(unittest.TestCase):
    """Request ids must increment, and never repeat."""

    def test_ids_increment(self) -> None:
        ids = jsonrpc.IdAllocator()
        self.assertEqual(ids.next_id(), 1)
        self.assertEqual(ids.next_id(), 2)
        self.assertEqual(ids.next_id(), 3)

    def test_ids_start_value(self) -> None:
        ids = jsonrpc.IdAllocator(start=100)
        self.assertEqual(ids.next_id(), 100)
        self.assertEqual(ids.next_id(), 101)


class SessionRequestIdTest(unittest.TestCase):
    def test_send_request_ids_increment(self) -> None:
        sent: list[bytes] = []
        session = jsonrpc.Session(send=sent.append)
        id1 = session.send_request("tools/list", {})
        id2 = session.send_request("tools/list", {})
        id3 = session.send_request("tools/call", {"name": "x"})
        self.assertEqual([id1, id2, id3], [1, 2, 3])
        self.assertEqual(len(sent), 3)


class SessionMatchingTest(unittest.TestCase):
    """Request/response matching by id, including out-of-order arrival."""

    def test_request_response_matching(self) -> None:
        sent: list[bytes] = []
        session = jsonrpc.Session(send=sent.append)
        request_id = session.send_request("tools/list", {})

        response = jsonrpc.encode_message(
            {"jsonrpc": "2.0", "id": request_id, "result": {"tools": []}}
        )
        session.handle_message(response)

        result = session.wait_response(request_id, timeout=1.0)
        self.assertEqual(result, {"tools": []})

    def test_out_of_order_matching(self) -> None:
        """Responses may arrive out of order; each must resolve its own id."""
        sent: list[bytes] = []
        session = jsonrpc.Session(send=sent.append)
        id1 = session.send_request("a", {})
        id2 = session.send_request("b", {})

        # Respond to the second request first.
        session.handle_message(
            jsonrpc.encode_message({"jsonrpc": "2.0", "id": id2, "result": "second"})
        )
        session.handle_message(
            jsonrpc.encode_message({"jsonrpc": "2.0", "id": id1, "result": "first"})
        )

        self.assertEqual(session.wait_response(id2, timeout=1.0), "second")
        self.assertEqual(session.wait_response(id1, timeout=1.0), "first")

    def test_error_response_raises_remote_error(self) -> None:
        sent: list[bytes] = []
        session = jsonrpc.Session(send=sent.append)
        request_id = session.send_request("tools/call", {})
        session.handle_message(
            jsonrpc.encode_message(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "error": {"code": -32000, "message": "boom"},
                }
            )
        )
        with self.assertRaises(jsonrpc.RemoteError) as ctx:
            session.wait_response(request_id, timeout=1.0)
        self.assertEqual(ctx.exception.code, -32000)
        self.assertEqual(ctx.exception.message, "boom")

    def test_unsolicited_response_does_not_raise(self) -> None:
        """A response to an id that was never requested must not crash the client."""
        sent: list[bytes] = []
        session = jsonrpc.Session(send=sent.append)
        # No request was ever sent with id 999; this must be a silent no-op.
        session.handle_message(jsonrpc.encode_message({"jsonrpc": "2.0", "id": 999, "result": "?"}))

    def test_notification_is_queued_not_dropped(self) -> None:
        sent: list[bytes] = []
        session = jsonrpc.Session(send=sent.append)
        session.handle_message(
            jsonrpc.encode_message(
                {"jsonrpc": "2.0", "method": "notifications/progress", "params": {}}
            )
        )
        notifications = session.drain_notifications()
        self.assertEqual(len(notifications), 1)
        self.assertEqual(notifications[0]["method"], "notifications/progress")
        # Draining again should come back empty.
        self.assertEqual(session.drain_notifications(), [])

    def test_wait_response_timeout(self) -> None:
        sent: list[bytes] = []
        session = jsonrpc.Session(send=sent.append)
        request_id = session.send_request("tools/list", {})
        with self.assertRaises(TimeoutError):
            session.wait_response(request_id, timeout=0.05)


class OversizedMessageTest(unittest.TestCase):
    """Messages over `MAX_MESSAGE_SIZE` must be rejected, not merely slow."""

    def test_encode_rejects_oversized_message(self) -> None:
        huge_params = {"data": "a" * (jsonrpc.MAX_MESSAGE_SIZE + 1)}
        with self.assertRaises(jsonrpc.ProtocolError):
            jsonrpc.encode_message(jsonrpc.make_request("x", huge_params, 1))

    def test_decode_rejects_oversized_message(self) -> None:
        huge = (
            b'{"jsonrpc": "2.0", "id": 1, "result": "'
            + b"a" * (jsonrpc.MAX_MESSAGE_SIZE + 1)
            + b'"}'
        )
        with self.assertRaises(jsonrpc.ProtocolError):
            jsonrpc.decode_message(huge)

    def test_message_at_the_limit_is_accepted(self) -> None:
        # The cap rejects messages that *exceed* it, not ones that meet it.
        obj = {"jsonrpc": "2.0", "id": 1, "method": "noop"}
        encoded = jsonrpc.encode_message(obj)
        self.assertLessEqual(len(encoded), jsonrpc.MAX_MESSAGE_SIZE)
        decoded = jsonrpc.decode_message(encoded)
        self.assertEqual(decoded["method"], "noop")


class MalformedMessageTest(unittest.TestCase):
    """Malformed input must raise `ProtocolError`, never crash the process."""

    def test_invalid_json_raises_protocol_error(self) -> None:
        with self.assertRaises(jsonrpc.ProtocolError):
            jsonrpc.decode_message(b"{not valid json")

    def test_invalid_json_does_not_leak_json_decode_error(self) -> None:
        # Callers should only ever need to catch `ProtocolError`, never a
        # raw `json.JSONDecodeError` (or other stdlib exception).
        try:
            jsonrpc.decode_message(b"{not valid json")
        except jsonrpc.ProtocolError:
            pass
        except json.JSONDecodeError:
            self.fail("decode_message() leaked a raw json.JSONDecodeError")

    def test_non_object_json_is_rejected(self) -> None:
        with self.assertRaises(jsonrpc.ProtocolError):
            jsonrpc.decode_message(b"[1, 2, 3]")

    def test_missing_jsonrpc_version_is_rejected(self) -> None:
        with self.assertRaises(jsonrpc.ProtocolError):
            jsonrpc.decode_message(json.dumps({"id": 1, "result": {}}).encode("utf-8"))

    def test_wrong_jsonrpc_version_is_rejected(self) -> None:
        with self.assertRaises(jsonrpc.ProtocolError):
            jsonrpc.decode_message(
                json.dumps({"jsonrpc": "1.0", "id": 1, "result": {}}).encode("utf-8")
            )

    def test_empty_bytes_do_not_crash(self) -> None:
        with self.assertRaises(jsonrpc.ProtocolError):
            jsonrpc.decode_message(b"")


def main() -> None:
    argv = [sys.argv[0]]
    if "--" in sys.argv:
        argv.extend(sys.argv[sys.argv.index("--") + 1:])
    unittest.main(argv=argv)


if __name__ == "__main__":
    main()
