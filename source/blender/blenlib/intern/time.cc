/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_time.hh"

#include <chrono>

#include <thread>
#include <unistd.h>

namespace blender {

double BLI_time_now_seconds()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t BLI_time_now_seconds_i()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void BLI_time_sleep_ms(int ms)
{
  if (ms >= 1000) {
    sleep(ms / 1000);
    ms = (ms % 1000);
  }

  usleep(ms * 1000);
}

void BLI_time_sleep_precise_us(int us)
{
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

}  // namespace blender
