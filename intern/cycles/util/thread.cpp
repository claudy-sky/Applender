/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/thread.h"

CCL_NAMESPACE_BEGIN

thread::thread(std::function<void()> run_cb) : run_cb_(run_cb), joined_(false)
{
  /* Set the stack size to 2MB to match GLIBC. The default 512KB on macOS is
   * too small for Embree, and consistent stack size also makes things more
   * predictable in general. */
  pthread_attr_t attribute;
  pthread_attr_init(&attribute);
  pthread_attr_setstacksize(&attribute, 1024 * 1024 * 2);
  pthread_create(&pthread_id, &attribute, run, (void *)this);
}

thread::~thread()
{
  if (!joined_) {
    join();
  }
}

void *thread::run(void *arg)
{
  thread *self = (thread *)(arg);
  self->run_cb_();
  return nullptr;
}

bool thread::join()
{
  joined_ = true;
  return pthread_join(pthread_id, nullptr) == 0;
}

CCL_NAMESPACE_END
