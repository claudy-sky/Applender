# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Minimal operator surface for the native MCP (Model Context Protocol)
client.

v1 deliberately ships no panel: connecting to a server, calling a tool,
and disconnecting are exposed as three thin operators (`mcp.connect`,
`mcp.disconnect`, `mcp.call_tool`), and server configuration is a JSON
file (see `_bpy_internal.mcp.default_config_path()` /
`_bpy_internal.mcp.configure_from_file()`). Scripts that want the client
directly, without going through an operator, can call `bpy.mcp` (the
native binding) or `_bpy_internal.mcp` (the Python client it wraps).

Background I/O (reading responses from a connected server) happens on
worker threads owned by `_bpy_internal.mcp`; `pump()` is registered here
as a `bpy.app.timers` callback so the UI thread periodically drains
whatever arrived, without ever blocking on it.
"""

from __future__ import annotations

import json

import bpy
from bpy.types import Operator
from bpy.props import StringProperty

# `_bpy_internal.mcp` is the actual client (transport + JSON parsing).
# These operators are thin wrappers around it, per the architecture: the
# heavy lifting is not reimplemented here.
import _bpy_internal.mcp as mcp_client


class MCP_OT_connect(Operator):
    """Connect to a configured MCP server and perform the initialize handshake"""
    bl_idname = "mcp.connect"
    bl_label = "Connect to MCP Server"
    bl_options = {'REGISTER'}

    name: StringProperty(
        name="Server",
        description="Name of a configured MCP server (see the MCP server config file)",
    )

    def execute(self, context):
        try:
            ok = mcp_client.connect(self.name)
        except KeyError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}

        if not ok:
            self.report({'ERROR'}, "Could not connect to MCP server '%s'" % self.name)
            return {'CANCELLED'}

        self.report({'INFO'}, "Connected to MCP server '%s'" % self.name)
        return {'FINISHED'}


class MCP_OT_disconnect(Operator):
    """Disconnect from a connected MCP server"""
    bl_idname = "mcp.disconnect"
    bl_label = "Disconnect from MCP Server"
    bl_options = {'REGISTER'}

    name: StringProperty(
        name="Server",
        description="Name of a configured MCP server",
    )

    def execute(self, context):
        mcp_client.disconnect(self.name)
        self.report({'INFO'}, "Disconnected from MCP server '%s'" % self.name)
        return {'FINISHED'}


class MCP_OT_call_tool(Operator):
    """Call a tool on a connected MCP server"""
    bl_idname = "mcp.call_tool"
    bl_label = "Call MCP Tool"
    bl_options = {'REGISTER'}

    name: StringProperty(
        name="Server",
        description="Name of a connected MCP server",
    )
    tool: StringProperty(
        name="Tool",
        description="Tool name, as returned by list_tools()",
    )
    arguments_json: StringProperty(
        name="Arguments",
        description="JSON-encoded object of tool arguments",
        default="{}",
    )

    def execute(self, context):
        try:
            arguments = json.loads(self.arguments_json) if self.arguments_json else {}
        except json.JSONDecodeError as ex:
            self.report({'ERROR'}, "Malformed tool arguments JSON: %s" % ex)
            return {'CANCELLED'}

        if not isinstance(arguments, dict):
            self.report({'ERROR'}, "Tool arguments JSON must decode to an object")
            return {'CANCELLED'}

        try:
            mcp_client.call_tool(self.name, self.tool, arguments)
        except (RuntimeError, KeyError) as ex:
            self.report({'ERROR'}, "MCP tool call failed: %s" % ex)
            return {'CANCELLED'}

        return {'FINISHED'}


classes = (
    MCP_OT_connect,
    MCP_OT_disconnect,
    MCP_OT_call_tool,
)


def register():
    # `pump()` already matches the `bpy.app.timers` callback signature
    # (no arguments, returns the next interval in seconds), so it is
    # registered directly rather than through a wrapper.
    if not bpy.app.timers.is_registered(mcp_client.pump):
        bpy.app.timers.register(
            mcp_client.pump,
            first_interval=0.5,
            persistent=True,
        )


def unregister():
    if bpy.app.timers.is_registered(mcp_client.pump):
        bpy.app.timers.unregister(mcp_client.pump)
