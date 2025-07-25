/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 */

#include "BLI_utildefines.h"
#include <Python.h>

#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

#include "bpy_app_opensubdiv.hh"

#include "../generic/py_capi_utils.hh"

#ifdef WITH_OPENSUBDIV
#  include "opensubdiv_capi.hh"
#endif

static PyTypeObject BlenderAppOpenSubdivType;

static PyStructSequence_Field app_opensubdiv_info_fields[] = {
    {"supported", "Boolean, True when Blender is built with OpenSubdiv support"},
    {"version", "The OpenSubdiv version as a tuple of 3 numbers"},
    {"version_string", "The OpenSubdiv version formatted as a string"},
    {nullptr},
};

static PyStructSequence_Desc app_opensubdiv_info_desc = {
    /*name*/ "bpy.app.opensubdiv",
    /*doc*/ "This module contains information about OpenSubdiv blender is linked against",
    /*fields*/ app_opensubdiv_info_fields,
    /*n_in_sequence*/ ARRAY_SIZE(app_opensubdiv_info_fields) - 1,
};

static PyObject *make_opensubdiv_info()
{
  PyObject *opensubdiv_info;
  int pos = 0;

  opensubdiv_info = PyStructSequence_New(&BlenderAppOpenSubdivType);
  if (opensubdiv_info == nullptr) {
    return nullptr;
  }

#ifndef WITH_OPENSUBDIV
#  define SetStrItem(str) \
    PyStructSequence_SET_ITEM(opensubdiv_info, pos++, PyUnicode_FromString(str))
#endif

#define SetObjItem(obj) PyStructSequence_SET_ITEM(opensubdiv_info, pos++, obj)

#ifdef WITH_OPENSUBDIV
  const int curversion = openSubdiv_getVersionHex();
  SetObjItem(PyBool_FromLong(1));
  SetObjItem(PyC_Tuple_Pack_I32({curversion / 10000, (curversion / 100) % 100, curversion % 100}));
  SetObjItem(PyUnicode_FromFormat(
      "%2d, %2d, %2d", curversion / 10000, (curversion / 100) % 100, curversion % 100));
#else
  SetObjItem(PyBool_FromLong(0));
  SetObjItem(PyC_Tuple_Pack_I32({0, 0, 0}));
  SetStrItem("Unknown");
#endif

  if (UNLIKELY(PyErr_Occurred())) {
    Py_DECREF(opensubdiv_info);
    return nullptr;
  }

#undef SetStrItem
#undef SetObjItem

  return opensubdiv_info;
}

PyObject *BPY_app_opensubdiv_struct()
{
  PyObject *ret;

  PyStructSequence_InitType(&BlenderAppOpenSubdivType, &app_opensubdiv_info_desc);

  ret = make_opensubdiv_info();

  /* prevent user from creating new instances */
  BlenderAppOpenSubdivType.tp_init = nullptr;
  BlenderAppOpenSubdivType.tp_new = nullptr;
  /* Without this we can't do `set(sys.modules)` #29635. */
  BlenderAppOpenSubdivType.tp_hash = (hashfunc)Py_HashPointer;

  return ret;
}
