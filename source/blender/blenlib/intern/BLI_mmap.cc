/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_mmap.hh"
#include "BLI_assert.hh"
#include "BLI_fileops.hh"
#include "BLI_mutex.hh"
#include "BLI_string_utils.hh"
#include "BLI_vector.hh"
#include "MEM_guardedalloc.h"

#include <atomic>
#include <cstring>

#include <csignal>
#include <cstdlib>
#include <sys/mman.h> /* For `mmap`. */
#include <unistd.h>   /* For `write`. */

namespace blender {

struct BLI_mmap_file {
  /* The address to which the file was mapped. */
  char *memory;

  /* The length of the file (and therefore the mapped region). */
  size_t length;

  /* Platform-specific handle for the mapping. */
  void *volatile handle;

  /* Flag to indicate IO errors. Needs to be volatile since it's being set from
   * within the signal handler, which is not part of the normal execution flow. */
  volatile bool io_error;

  /* Used to break out of infinite loops when an error keeps occurring.
   * See the comments in #try_handle_error_for_address for details. */
  size_t id;
};

/* General mutex used to protect access to the list of open mapped files, ensure the handler is
 * initialized only once and to prevent multiple threads from trying to remap the same
 * memory-mapped region in parallel. */
static Mutex mmap_mutex;

/* When using memory-mapped files, any IO errors will result in an EXCEPTION_IN_PAGE_ERROR on
 * Windows and a SIGBUS signal on other platforms. Therefore, we need to catch that signal and
 * stop reading the file in question. To do so, we keep a list of all currently opened
 * memory-mapped files, and if a error is caught, we check if the failed address is inside one of
 * the mapped regions. If it is, we set a flag to indicate a failed read and remap the memory in
 * question to a zero-backed region in order to avoid additional signals. The code that actually
 * reads the memory area has to check whether the flag was set after it's done reading. If the
 * error occurred outside of a memory-mapped region or the remapping failed, we call the previous
 * handler if one was initialized and abort the process otherwise on Linux and on Windows let the
 * exception crash the program. */
static Vector<BLI_mmap_file *> &open_mmaps_vector()
{
  static Vector<BLI_mmap_file *> open_mmaps;
  return open_mmaps;
}

/* Print a message to the STDERR without using the standard library routines.
 * If a MMAP error occurs while reading a pointer inside one of the standard library's IO routines,
 * any global locks it was holding won't be unlocked when entering the handler.
 * Using the normal printing routines could then cause a deadlock. */
static void print_error(const char *message);

/* Tries to replace the mapping with zeroes.
 * Returns true on success. */
static bool try_map_zeros(BLI_mmap_file *file);

/* Find the file mapping containing the address and call #try_map_zeroes for it.
 * Returns true when execution can continue. */
static bool try_handle_error_for_address(const void *address)
{
  static thread_local size_t last_handled_file_id = -1;

  std::unique_lock lock(mmap_mutex);

  BLI_mmap_file *file = nullptr;
  for (BLI_mmap_file *link_file : open_mmaps_vector()) {
    /* Is the address where the error occurred in this file's mapped range? */
    if (address >= link_file->memory && address < link_file->memory + link_file->length) {
      file = link_file;
      break;
    }
  }

  if (file == nullptr) {
    /* Not our error. */
    return false;
  }

  /* Check if we already handled this error. */
  if (file->io_error) {
    /* If `file->io_error` is true, either a different thread has
     * already replaced the mapping after this thread raised the
     * exception, but before we got the lock, and execution can
     * continue, or replacing the mapping did not avoid the current
     * exception. We need to check if continuing execution fails to
     * avoid an infinite loop in the second case. To detect such a
     * situation, the last handled mapping's ID is stored per thread and
     * compared against it to see if continuing execution was already
     * tried for this mapping in this thread. If that is the case,
     * forward the exception instead of continuing execution again. As
     * multiple threads could encounter an exception for the same
     * mapping at the same time, a boolean stored in `BLI_mmap_file`
     * would not work for this detection, as the condition we need to
     * detect is thread dependent. */
    if (file->id == last_handled_file_id) {
      /* Some possible causes of the error below are:
       * - Thread safety issues in the error handling code.
       * - Faulty remapping without having signaled an error in `try_map_zeros`.
       * - Invalid usage of an address in the mapped range, such as
       *   unaligned access on some platforms.
       */
      print_error(
          "Error: Unexpected exception in mapped file which was already remapped with zeros.");
      return false;
    }
    /* Another thread has already remapped the range, we can continue execution. */
    last_handled_file_id = file->id;
    return true;
  }

  last_handled_file_id = file->id;
  file->io_error = true;

  if (!try_map_zeros(file)) {
    print_error("Error: Could not replace mapped file with zeros.");
    return false;
  }

  return true;
}

static void print_error(const char *message)
{
  char buffer[256];
  size_t length = BLI_string_join(buffer, sizeof(buffer), "BLI_mmap: ", message, "\n");
  if (write(STDERR_FILENO, buffer, length) < 0) {
    /* If writing to stderr fails, there is nowhere to write an error about that. */
  }
}

static bool try_map_zeros(BLI_mmap_file *file)
{
  /* Replace the mapped memory with zeroes. */
  const void *mapped_memory = mmap(
      file->memory, file->length, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (mapped_memory == MAP_FAILED) {
    return false;
  }

  return true;
}

static struct sigaction next_handler = {};

static void sigbus_handler(int sig, siginfo_t *siginfo, void *ptr) noexcept
{
  /* We only handle SIGBUS here for now. */
  BLI_assert(sig == SIGBUS);

  if (try_handle_error_for_address(siginfo->si_addr)) {
    return;
  }

  /* Fall back to the other handler if there was one.
   *
   * No lock is needed here, as #try_handle_error_for_address
   * unconditionally locks `mmap_mutex`, and as such
   * #ensure_mmap_initialized must have finished and #next_handler
   * will be set up. */
  if (next_handler.sa_sigaction && (next_handler.sa_flags & SA_SIGINFO)) {
    next_handler.sa_sigaction(sig, siginfo, ptr);
  }
  else if (!ELEM(next_handler.sa_handler, nullptr, SIG_DFL, SIG_IGN)) {
    next_handler.sa_handler(sig);
  }
  else {
    print_error("Unhandled SIGBUS caught");
    abort();
  }
}

/* Ensures that the error handler is set up and ready. */
static bool ensure_mmap_initialized()
{
  static std::atomic_bool initialized = false;
  if (initialized) {
    return true;
  }

  std::unique_lock lock(mmap_mutex);
  if (!initialized) {
    struct sigaction newact = {{nullptr}}, oldact = {{nullptr}};

    newact.sa_sigaction = sigbus_handler;
    newact.sa_flags = SA_SIGINFO;

    if (sigaction(SIGBUS, &newact, &oldact)) {
      return false;
    }

    /* Remember the previous handler to fall back to it if the error
     * does not belong to any of the mapped files. */
    next_handler = oldact;
    initialized = true;
  }

  return true;
}

/* Adds a file to the list that the error handler checks. */
static void error_handler_add(BLI_mmap_file *file)
{
  std::unique_lock lock(mmap_mutex);
  open_mmaps_vector().append(file);
}

/* Removes a file from the list that the error handler checks. */
static void error_handler_remove(BLI_mmap_file *file)
{
  std::unique_lock lock(mmap_mutex);
  open_mmaps_vector().remove_first_occurrence_and_reorder(file);
}

BLI_mmap_file *BLI_mmap_open(int fd)
{
  static std::atomic_size_t id_counter = 0;

  void *memory, *handle = nullptr;
  /* It's important not to use `BLI_lseek` here because in case we fail to use MMAP and returning
   * nullptr, the calling function is not closing the file using the file descriptor means next
   * time the file is read the file-pointer is still pointing to the end. This is not an issue for
   * blender but it throws off the virtual file-system when blender is running on the
   * render-network. See PR !155823 for details. */
  const size_t length = BLI_file_descriptor_size(fd);
  if (length == size_t(-1)) [[unlikely]] {
    return nullptr;
  }

  /* Ensures that the error handler is set up and ready. */
  if (!ensure_mmap_initialized()) {
    return nullptr;
  }

  /* Map the given file to memory. */
  memory = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
  if (memory == MAP_FAILED) {
    return nullptr;
  }

  /* Now that the mapping was successful, allocate memory and set up the #BLI_mmap_file. */
  BLI_mmap_file *file = MEM_new_zeroed<BLI_mmap_file>(__func__);
  file->memory = static_cast<char *>(memory);
  file->handle = handle;
  file->length = length;
  file->id = id_counter++;

  /* Register the file with the error handler. */
  error_handler_add(file);

  return file;
}

bool BLI_mmap_read(BLI_mmap_file *file, void *dest, size_t offset, size_t length)
{
  /* If a previous read has already failed or we try to read past the end,
   * don't even attempt to read any further. */
  if (file->io_error || (offset + length > file->length)) {
    return false;
  }

  memcpy(dest, file->memory + offset, length);

  return !file->io_error;
}

void *BLI_mmap_get_pointer(BLI_mmap_file *file)
{
  return file->memory;
}

size_t BLI_mmap_get_length(const BLI_mmap_file *file)
{
  return file->length;
}

bool BLI_mmap_any_io_error(const BLI_mmap_file *file)
{
  return file->io_error;
}

void BLI_mmap_free(BLI_mmap_file *file)
{
  error_handler_remove(file);
  munmap(static_cast<void *>(file->memory), file->length);

  MEM_delete(file);
}

}  // namespace blender
