/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/task.h"

#include "util/log.h"
#include "util/time.h"

#ifdef __APPLE__
#  include <cstdlib>

#  include <pthread/qos.h>

#  include <tbb/task_scheduler_observer.h>
#endif

CCL_NAMESPACE_BEGIN

/* Task Pool */

TaskPool::TaskPool() : start_time(time_dt()), num_tasks_pushed(0)
{
  /* Workers executing this pool's tasks join the constructing thread's arena. */
  TaskScheduler::qos_observe_current_arena();
}

TaskPool::~TaskPool()
{
  cancel();
}

void TaskPool::push(TaskRunFunction &&task)
{
  tbb_group.run(std::move(task));
  num_tasks_pushed++;
}

void TaskPool::wait_work(Summary *stats)
{
  tbb_group.wait();

  if (stats != nullptr) {
    stats->time_total = time_dt() - start_time;
    stats->num_tasks_handled = num_tasks_pushed;
  }

  num_tasks_pushed = 0;
}

void TaskPool::cancel()
{
  if (num_tasks_pushed > 0) {
    tbb_group.cancel();
    tbb_group.wait();
    num_tasks_pushed = 0;
  }
}

bool TaskPool::canceled()
{
  return tbb::is_current_task_group_canceling();
}

/* Task Scheduler */

#ifdef __APPLE__
/* Classifies TBB worker threads as USER_INITIATED for the XNU scheduler:
 * eligible for performance cores at full timeshare, while the
 * USER_INTERACTIVE main (UI) thread still ranks ahead of them.
 *
 * NOTE: TBB workers are shared process-wide with the host application, and
 * the class sticks to a worker once applied. */
class QoSTaskSchedulerObserver : public tbb::task_scheduler_observer {
 public:
  QoSTaskSchedulerObserver()
  {
    observe(true);
  }

  ~QoSTaskSchedulerObserver() override
  {
    observe(false);
  }

  void on_scheduler_entry(bool is_worker) override
  {
    /* QoS only applies to the calling thread. Never re-classify non-worker
     * threads that join to wait on tasks: that could demote the main thread
     * below its USER_INTERACTIVE class. */
    if (!is_worker) {
      return;
    }
    /* Entry fires on every arena join, the call is idempotent. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
  }
};

static bool qos_disabled()
{
  /* Shares the kill-switch with `BLI_thread_qos_set` so one environment
   * variable disables all QoS assignment for bisecting. */
  static const bool disabled = getenv("BLENDER_THREAD_QOS_DISABLE") != nullptr;
  return disabled;
}
#endif

void TaskScheduler::qos_observe_current_arena()
{
#ifdef __APPLE__
  if (qos_disabled()) {
    return;
  }
  /* oneTBB scheduler observers are arena-scoped: a default-constructed
   * observer only sees workers joining the constructing thread's implicit
   * arena. Arm one per dispatching thread so workers executing that thread's
   * parallel work get classified. Deliberately leaked: thread/static teardown
   * order versus oneTBB shutdown is undefined, and `observe(false)` on a
   * destroyed scheduler would crash at exit. */
  thread_local QoSTaskSchedulerObserver *observer = new QoSTaskSchedulerObserver();
  (void)observer;
#endif
}

void TaskScheduler::qos_enter_render_thread()
{
#ifdef __APPLE__
  if (qos_disabled()) {
    return;
  }
  /* Only call on dedicated render/session threads: this re-classifies the
   * calling thread itself and must never run on the main (UI) thread. */
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
  qos_observe_current_arena();
#endif
}

thread_mutex TaskScheduler::mutex;
int TaskScheduler::users = 0;
int TaskScheduler::active_num_threads = 0;
unique_ptr<tbb::global_control> TaskScheduler::global_control;

void TaskScheduler::init(const int num_threads)
{
  const thread_scoped_lock lock(mutex);
  /* Multiple cycles instances can use this task scheduler, sharing the same
   * threads, so we keep track of the number of users. */
  ++users;
  if (users != 1) {
    return;
  }
  if (num_threads > 0) {
    /* Automatic number of threads. */
    LOG_INFO << "Overriding number of TBB threads to " << num_threads << ".";
    global_control = make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism,
                                                      num_threads);
    active_num_threads = num_threads;
  }
  else {
    active_num_threads = tbb::this_task_arena::max_concurrency();
  }
}

void TaskScheduler::exit()
{
  const thread_scoped_lock lock(mutex);
  users--;
  if (users == 0) {
    global_control.reset();
    active_num_threads = 0;
  }
}

void TaskScheduler::free_memory()
{
  assert(users == 0);
}

int TaskScheduler::max_concurrency()
{
  const thread_scoped_lock lock(mutex);
  return (users > 0) ? active_num_threads : tbb::this_task_arena::max_concurrency();
}

/* Dedicated Task Pool */

DedicatedTaskPool::DedicatedTaskPool()
{
  do_cancel = false;
  do_exit = false;
  num = 0;

  worker_thread = make_unique<thread>([this] { thread_run(); });
}

DedicatedTaskPool::~DedicatedTaskPool()
{
  wait();

  do_exit = true;
  queue_cond.notify_all();

  worker_thread->join();
  worker_thread.reset();
}

void DedicatedTaskPool::push(TaskRunFunction &&run, bool front)
{
  num_increase();

  /* add task to queue */
  queue_mutex.lock();
  if (front) {
    queue.emplace_front(std::move(run));
  }
  else {
    queue.emplace_back(std::move(run));
  }

  queue_cond.notify_one();
  queue_mutex.unlock();
}

void DedicatedTaskPool::wait()
{
  thread_scoped_lock num_lock(num_mutex);

  while (num) {
    num_cond.wait(num_lock);
  }
}

void DedicatedTaskPool::cancel()
{
  do_cancel = true;

  clear();
  wait();

  do_cancel = false;
}

bool DedicatedTaskPool::canceled()
{
  return do_cancel;
}

void DedicatedTaskPool::num_decrease(const int done)
{
  const thread_scoped_lock num_lock(num_mutex);
  num -= done;

  assert(num >= 0);
  if (num == 0) {
    num_cond.notify_all();
  }
}

void DedicatedTaskPool::num_increase()
{
  const thread_scoped_lock num_lock(num_mutex);
  num++;
  num_cond.notify_all();
}

bool DedicatedTaskPool::thread_wait_pop(TaskRunFunction &task)
{
  thread_scoped_lock queue_lock(queue_mutex);

  while (queue.empty() && !do_exit) {
    queue_cond.wait(queue_lock);
  }

  if (queue.empty()) {
    assert(do_exit);
    return false;
  }

  task = queue.front();
  queue.pop_front();

  return true;
}

void DedicatedTaskPool::thread_run()
{
  TaskRunFunction task;

  /* keep popping off tasks */
  while (thread_wait_pop(task)) {
    /* run task */
    task();

    /* delete task */
    task = nullptr;

    /* notify task was done */
    num_decrease(1);
  }
}

void DedicatedTaskPool::clear()
{
  thread_scoped_lock queue_lock(queue_mutex);

  /* erase all tasks from the queue */
  const int done = queue.size();
  queue.clear();

  queue_lock.unlock();

  /* notify done */
  num_decrease(done);
}

string TaskPool::Summary::full_report() const
{
  string report;
  report += string_printf("Total time:    %f\n", time_total);
  report += string_printf("Tasks handled: %d", num_tasks_handled);
  return report;
}

CCL_NAMESPACE_END
