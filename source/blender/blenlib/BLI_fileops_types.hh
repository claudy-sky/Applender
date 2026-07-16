/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 * \brief Some types for dealing with directories.
 */

#include <sys/stat.h>

namespace blender {

#define FILELIST_DIRENTRY_SIZE_LEN 16
#define FILELIST_DIRENTRY_MODE_LEN 4
#define FILELIST_DIRENTRY_OWNER_LEN 16
#define FILELIST_DIRENTRY_TIME_LEN 8
#define FILELIST_DIRENTRY_DATE_LEN 16

struct direntry {
  mode_t type;
  const char *relname;
  const char *path;
  struct stat s;
};

struct dirlink {
  struct dirlink *next, *prev;
  char *name;
};

}  // namespace blender
