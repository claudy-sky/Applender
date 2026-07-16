/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonmcp
 *
 * The `bpy.mcp` Python module: a thin binding on top of the native
 * `blender::mcp` facade (see `intern/mcp/MCP_client.hh`).
 *
 * Transport (stdio subprocess, HTTP) and all JSON parsing of data
 * received from external, untrusted MCP servers are delegated all the way
 * down to the embedded, memory-safe Python client `_bpy_internal.mcp` --
 * neither this module nor the native facade beneath it ever hand-parses
 * JSON. Methods that return server data return it as a JSON-encoded
 * Python `str`; script authors should decode it with the standard `json`
 * module.
 *
 * - Use `pymcp_` for local API.
 */

#include <Python.h>

#include <string>

#include "MCP_client.hh"

#include "mcp_py_api.hh" /* Own include. */

namespace blender {

/* -------------------------------------------------------------------- */
/** \name bpy.mcp Methods
 * \{ */

static PyObject *pystr_from_json_or_none(const std::string &json)
{
  if (json.empty()) {
    /* Empty string means "no data" - the facade fails soft rather than
     * raising, so mirror that here instead of returning an empty string
     * that might be mistaken for valid (empty) JSON. */
    Py_RETURN_NONE;
  }
  return PyUnicode_FromStringAndSize(json.data(), Py_ssize_t(json.size()));
}

PyDoc_STRVAR(
    /* Wrap. */
    pymcp_connect_doc,
    ".. function:: connect(name)\n"
    "\n"
    "   Start the transport for the named MCP server and perform the MCP\n"
    "   ``initialize`` handshake. Blocks the calling thread, bounded by\n"
    "   the client's own connect timeout.\n"
    "\n"
    "   :arg name: Name of a configured MCP server.\n"
    "   :type name: str\n"
    "   :return: True if the connection and handshake succeeded.\n"
    "   :rtype: bool\n");
static PyObject *pymcp_connect(PyObject * /*self*/, PyObject *args)
{
  const char *name;
  if (!PyArg_ParseTuple(args, "s:connect", &name)) {
    return nullptr;
  }
  if (mcp::connect(name)) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pymcp_disconnect_doc,
    ".. function:: disconnect(name)\n"
    "\n"
    "   Disconnect from the named MCP server, if currently connected.\n"
    "   A no-op if it is not connected.\n"
    "\n"
    "   :arg name: Name of a configured MCP server.\n"
    "   :type name: str\n");
static PyObject *pymcp_disconnect(PyObject * /*self*/, PyObject *args)
{
  const char *name;
  if (!PyArg_ParseTuple(args, "s:disconnect", &name)) {
    return nullptr;
  }
  mcp::disconnect(name);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pymcp_is_connected_doc,
    ".. function:: is_connected(name)\n"
    "\n"
    "   :arg name: Name of a configured MCP server.\n"
    "   :type name: str\n"
    "   :return: True if currently connected and initialized.\n"
    "   :rtype: bool\n");
static PyObject *pymcp_is_connected(PyObject * /*self*/, PyObject *args)
{
  const char *name;
  if (!PyArg_ParseTuple(args, "s:is_connected", &name)) {
    return nullptr;
  }
  if (mcp::is_connected(name)) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pymcp_list_servers_doc,
    ".. function:: list_servers()\n"
    "\n"
    "   :return: JSON array of configured (not necessarily connected) MCP\n"
    "      server names, or None on failure.\n"
    "   :rtype: str | None\n");
static PyObject *pymcp_list_servers(PyObject * /*self*/)
{
  return pystr_from_json_or_none(mcp::list_servers_json());
}

PyDoc_STRVAR(
    /* Wrap. */
    pymcp_list_tools_doc,
    ".. function:: list_tools(name)\n"
    "\n"
    "   :arg name: Name of a connected MCP server.\n"
    "   :type name: str\n"
    "   :return: JSON array of tool descriptors from ``tools/list``, or\n"
    "      None on failure.\n"
    "   :rtype: str | None\n");
static PyObject *pymcp_list_tools(PyObject * /*self*/, PyObject *args)
{
  const char *name;
  if (!PyArg_ParseTuple(args, "s:list_tools", &name)) {
    return nullptr;
  }
  return pystr_from_json_or_none(mcp::list_tools_json(name));
}

PyDoc_STRVAR(
    /* Wrap. */
    pymcp_call_tool_doc,
    ".. function:: call_tool(name, tool, arguments_json)\n"
    "\n"
    "   :arg name: Name of a connected MCP server.\n"
    "   :type name: str\n"
    "   :arg tool: Tool name, as returned by :func:`list_tools`.\n"
    "   :type tool: str\n"
    "   :arg arguments_json: JSON-encoded object of tool arguments.\n"
    "      Pass an empty string (or omit) for no arguments.\n"
    "   :type arguments_json: str\n"
    "   :return: JSON-encoded result of ``tools/call``, or None on\n"
    "      failure.\n"
    "   :rtype: str | None\n");
static PyObject *pymcp_call_tool(PyObject * /*self*/, PyObject *args)
{
  const char *name;
  const char *tool;
  const char *arguments_json = "";
  if (!PyArg_ParseTuple(args, "ss|s:call_tool", &name, &tool, &arguments_json)) {
    return nullptr;
  }
  return pystr_from_json_or_none(mcp::call_tool_json(name, tool, arguments_json));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name bpy.mcp Module
 * \{ */

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

static PyMethodDef pymcp_methods[] = {
    {"connect", reinterpret_cast<PyCFunction>(pymcp_connect), METH_VARARGS, pymcp_connect_doc},
    {"disconnect",
     reinterpret_cast<PyCFunction>(pymcp_disconnect),
     METH_VARARGS,
     pymcp_disconnect_doc},
    {"is_connected",
     reinterpret_cast<PyCFunction>(pymcp_is_connected),
     METH_VARARGS,
     pymcp_is_connected_doc},
    {"list_servers",
     reinterpret_cast<PyCFunction>(pymcp_list_servers),
     METH_NOARGS,
     pymcp_list_servers_doc},
    {"list_tools",
     reinterpret_cast<PyCFunction>(pymcp_list_tools),
     METH_VARARGS,
     pymcp_list_tools_doc},
    {"call_tool",
     reinterpret_cast<PyCFunction>(pymcp_call_tool),
     METH_VARARGS,
     pymcp_call_tool_doc},
    {nullptr, nullptr, 0, nullptr},
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

PyDoc_STRVAR(
    /* Wrap. */
    pymcp_doc,
    "This module provides access to the native MCP (Model Context Protocol)\n"
    "client.\n"
    "\n"
    "It is a thin binding: transport (stdio subprocess, HTTP) and all JSON\n"
    "parsing of data received from external MCP servers is implemented by\n"
    "the embedded, memory-safe Python client ``_bpy_internal.mcp`` -- this\n"
    "module never hand-parses untrusted JSON itself. Functions that return\n"
    "server data return a JSON-encoded string; decode it with the standard\n"
    "``json`` module.\n");
static PyModuleDef pymcp_module_def = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "mcp",
    /*m_doc*/ pymcp_doc,
    /*m_size*/ 0,
    /*m_methods*/ pymcp_methods,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

PyObject *BPyInit_mcp()
{
  return PyModule_Create(&pymcp_module_def);
}

/** \} */

}  // namespace blender
