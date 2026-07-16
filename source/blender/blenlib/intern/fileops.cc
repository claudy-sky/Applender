/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_fileops.hh"

namespace blender {

fstream::fstream(const char *filepath, std::ios_base::openmode mode)
{
  this->open(filepath, mode);
}

fstream::fstream(const std::string &filepath, std::ios_base::openmode mode)
{
  this->open(filepath, mode);
}

void fstream::open(StringRefNull filepath, ios_base::openmode mode)
{
  std::fstream::open(filepath, mode);
}

}  // namespace blender
