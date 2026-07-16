/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_mcp
 *
 * Implementation of the `blender::mcp` native facade. Every function
 * forwards to the embedded Python module `_bpy_internal.mcp`, which owns
 * transport and JSON parsing (see `MCP_client.hh` for the full rationale).
 *
 * GIL handling follows the pattern used elsewhere in Blender for calling
 * into Python from arbitrary native threads, e.g.
 * `source/blender/python/intern/bpy_interface.cc` (`PyGILState_Ensure()` /
 * `PyGILState_Release()`, rather than assuming the calling thread already
 * holds the GIL).
 */

#include "MCP_client.hh"

#include <Python.h>

namespace blender::mcp {

namespace {

/** RAII GIL guard: acquire on construction, release on destruction. */
class GilLock {
 public:
  GilLock() : state_(PyGILState_Ensure()) {}
  ~GilLock()
  {
    PyGILState_Release(state_);
  }

  GilLock(const GilLock &) = delete;
  GilLock &operator=(const GilLock &) = delete;

 private:
  PyGILState_STATE state_;
};

/** Print and clear any pending Python exception. Caller must hold the GIL. */
void log_and_clear_error()
{
  if (PyErr_Occurred()) {
    PyErr_Print();
  }
}

/**
 * Import `_bpy_internal.mcp`, returning a new reference, or `nullptr`
 * with the Python error printed and cleared. Caller must hold the GIL.
 */
PyObject *import_mcp_module()
{
  PyObject *mod = PyImport_ImportModule("_bpy_internal.mcp");
  if (mod == nullptr) {
    log_and_clear_error();
  }
  return mod;
}

/** Convert a Python `str` to a UTF-8 `std::string`. Borrows `obj`. */
bool py_object_to_string(PyObject *obj, std::string &r_result)
{
  if (obj == nullptr || !PyUnicode_Check(obj)) {
    return false;
  }
  Py_ssize_t size = 0;
  const char *data = PyUnicode_AsUTF8AndSize(obj, &size);
  if (data == nullptr) {
    log_and_clear_error();
    return false;
  }
  r_result.assign(data, size_t(size));
  return true;
}

/**
 * JSON-encode `obj` (via the stdlib `json` module, never hand-rolled
 * C++). Borrows `obj`. Returns an empty string on any failure.
 * Caller must hold the GIL.
 */
std::string json_dumps(PyObject *obj)
{
  if (obj == nullptr) {
    return {};
  }
  PyObject *json_mod = PyImport_ImportModule("json");
  if (json_mod == nullptr) {
    log_and_clear_error();
    return {};
  }
  PyObject *dumped = PyObject_CallMethod(json_mod, "dumps", "O", obj);
  Py_DECREF(json_mod);
  if (dumped == nullptr) {
    log_and_clear_error();
    return {};
  }
  std::string result;
  py_object_to_string(dumped, result);
  Py_DECREF(dumped);
  return result;
}

/**
 * Parse `json_text` (via the stdlib `json` module) into a new Python
 * object reference. An empty `json_text` is treated as `{}`. Returns
 * `nullptr` on failure. Caller must hold the GIL and own the result
 * (DECREF it).
 */
PyObject *json_loads(const std::string &json_text)
{
  if (json_text.empty()) {
    return PyDict_New();
  }
  PyObject *json_mod = PyImport_ImportModule("json");
  if (json_mod == nullptr) {
    log_and_clear_error();
    return nullptr;
  }
  PyObject *parsed = PyObject_CallMethod(json_mod, "loads", "s", json_text.c_str());
  Py_DECREF(json_mod);
  if (parsed == nullptr) {
    log_and_clear_error();
    return nullptr;
  }
  return parsed;
}

}  // namespace

bool is_available()
{
  if (!Py_IsInitialized()) {
    return false;
  }
  GilLock gil;
  PyObject *mod = import_mcp_module();
  if (mod == nullptr) {
    return false;
  }
  Py_DECREF(mod);
  return true;
}

std::string list_servers_json()
{
  if (!Py_IsInitialized()) {
    return {};
  }
  GilLock gil;
  PyObject *mod = import_mcp_module();
  if (mod == nullptr) {
    return {};
  }
  PyObject *result = PyObject_CallMethod(mod, "list_servers", nullptr);
  Py_DECREF(mod);
  if (result == nullptr) {
    log_and_clear_error();
    return {};
  }
  std::string json = json_dumps(result);
  Py_DECREF(result);
  return json;
}

bool connect(const std::string &name)
{
  if (!Py_IsInitialized()) {
    return false;
  }
  GilLock gil;
  PyObject *mod = import_mcp_module();
  if (mod == nullptr) {
    return false;
  }
  PyObject *result = PyObject_CallMethod(mod, "connect", "s", name.c_str());
  Py_DECREF(mod);
  if (result == nullptr) {
    log_and_clear_error();
    return false;
  }
  const bool ok = PyObject_IsTrue(result) == 1;
  Py_DECREF(result);
  return ok;
}

void disconnect(const std::string &name)
{
  if (!Py_IsInitialized()) {
    return;
  }
  GilLock gil;
  PyObject *mod = import_mcp_module();
  if (mod == nullptr) {
    return;
  }
  PyObject *result = PyObject_CallMethod(mod, "disconnect", "s", name.c_str());
  Py_DECREF(mod);
  if (result == nullptr) {
    log_and_clear_error();
    return;
  }
  Py_DECREF(result);
}

bool is_connected(const std::string &name)
{
  if (!Py_IsInitialized()) {
    return false;
  }
  GilLock gil;
  PyObject *mod = import_mcp_module();
  if (mod == nullptr) {
    return false;
  }
  PyObject *result = PyObject_CallMethod(mod, "is_connected", "s", name.c_str());
  Py_DECREF(mod);
  if (result == nullptr) {
    log_and_clear_error();
    return false;
  }
  const bool ok = PyObject_IsTrue(result) == 1;
  Py_DECREF(result);
  return ok;
}

std::string list_tools_json(const std::string &name)
{
  if (!Py_IsInitialized()) {
    return {};
  }
  GilLock gil;
  PyObject *mod = import_mcp_module();
  if (mod == nullptr) {
    return {};
  }
  PyObject *result = PyObject_CallMethod(mod, "list_tools", "s", name.c_str());
  Py_DECREF(mod);
  if (result == nullptr) {
    log_and_clear_error();
    return {};
  }
  std::string json = json_dumps(result);
  Py_DECREF(result);
  return json;
}

std::string call_tool_json(const std::string &name,
                            const std::string &tool,
                            const std::string &arguments_json)
{
  if (!Py_IsInitialized()) {
    return {};
  }
  GilLock gil;
  PyObject *mod = import_mcp_module();
  if (mod == nullptr) {
    return {};
  }
  PyObject *arguments = json_loads(arguments_json);
  if (arguments == nullptr) {
    Py_DECREF(mod);
    return {};
  }
  PyObject *result = PyObject_CallMethod(
      mod, "call_tool", "ssO", name.c_str(), tool.c_str(), arguments);
  Py_DECREF(arguments);
  Py_DECREF(mod);
  if (result == nullptr) {
    log_and_clear_error();
    return {};
  }
  std::string json = json_dumps(result);
  Py_DECREF(result);
  return json;
}

}  // namespace blender::mcp
