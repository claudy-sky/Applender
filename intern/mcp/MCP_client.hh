/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_mcp
 *
 * Native C++ facade for the MCP (Model Context Protocol) client.
 *
 * This is the native entry point for MCP support: it exists so future
 * native callers (native UI, operators implemented in C++) can talk to
 * MCP servers without going through the `bpy.mcp` Python module.
 *
 * It is intentionally a THIN facade. All transport (stdio subprocess,
 * HTTP) and all JSON parsing live in the embedded Python module
 * `_bpy_internal.mcp` (see `scripts/modules/_bpy_internal/mcp/`).
 * Untrusted JSON received from external MCP servers is parsed there, by
 * Python's memory-safe standard library `json` module -- never by
 * hand-rolled C++ in this facade.
 *
 * Every function below marshals plain strings in and out, forwarding to
 * `_bpy_internal.mcp` via the CPython C-API (see `intern/MCP_client.cc`).
 * JSON payloads returned as `std::string` are produced by `json.dumps()`
 * on the Python side; this facade does not itself parse them, that is up
 * to the caller (e.g. `bpy.mcp`, which hands the string back to Python
 * script authors to `json.loads()` themselves).
 *
 * All functions fail soft: an empty string / `false` return means "not
 * available, not connected, or the call failed" -- never an exception or
 * a crash. Errors raised on the Python side are printed to the console
 * (via `PyErr_Print()`) and cleared.
 */

#pragma once

#include <string>

namespace blender::mcp {

/**
 * True when the embedded Python interpreter is running and the
 * `_bpy_internal.mcp` module can be imported, i.e. every other function
 * in this file has a chance of doing something useful. Every other
 * function already fails soft when this would be false, so checking it
 * explicitly is optional.
 */
bool is_available();

/** JSON array of configured (not necessarily connected) MCP server names. */
std::string list_servers_json();

/**
 * Start the transport for server `name` and perform the MCP `initialize`
 * handshake. Blocks the calling thread, bounded by the Python client's
 * own connect timeout. Returns true on success.
 */
bool connect(const std::string &name);

/** Disconnect and release resources for server `name`, if connected. */
void disconnect(const std::string &name);

/** True if `name` is currently connected and initialized. */
bool is_connected(const std::string &name);

/** JSON array of tool descriptors, from `tools/list`, for server `name`. */
std::string list_tools_json(const std::string &name);

/**
 * Call `tool` on server `name`.
 *
 * `arguments_json` is a JSON object (as text); pass an empty string for
 * "no arguments". Returns the JSON-encoded result of `tools/call`.
 */
std::string call_tool_json(const std::string &name,
                            const std::string &tool,
                            const std::string &arguments_json);

}  // namespace blender::mcp
