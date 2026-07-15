/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Some really low-level file operations.
 */

#include <cstdio>
#include <cstdlib>
#include <sys/types.h>

#include <sys/stat.h>

#if defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__HAIKU__)
/* Other modern unix OS's should probably use this also. */
#  include <sys/statvfs.h>
#  define USE_STATFS_STATVFS
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__)
/* For statfs */
#  include <sys/mount.h>
#  include <sys/param.h>
#endif

#if defined(__linux__) || defined(__hpux) || defined(__GNU__) || defined(__GLIBC__)
#  include <sys/vfs.h>
#endif

#include <cstring>
#include <fcntl.h>

#include <pwd.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* lib includes */
#include "MEM_guardedalloc.h"

#include "BLI_fileops.hh"
#include "BLI_linklist.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_threads.hh"
#include "BLI_utildefines.hh"

namespace blender {

/* NOTE: The implementation for Apple lives in storage_apple.mm. */
#if !defined(__APPLE__)
bool BLI_change_working_dir(const char *dir)
{
  BLI_assert(BLI_thread_is_main());

  if (!BLI_is_dir(dir)) {
    return false;
  }
#  if defined(WIN32)
  wchar_t wdir[FILE_MAX];
  if (conv_utf_8_to_16(dir, wdir, ARRAY_SIZE(wdir)) != 0) {
    return false;
  }
  return _wchdir(wdir) == 0;
#  else
  return chdir(dir) == 0;
#  endif
}

char *BLI_current_working_dir(char *dir, const size_t maxncpy)
{
#  if defined(WIN32)
  wchar_t path[MAX_PATH];
  if (_wgetcwd(path, MAX_PATH)) {
    if (BLI_strncpy_wchar_as_utf8(dir, path, maxncpy) != maxncpy) {
      return dir;
    }
  }
  return nullptr;
#  else
  return getcwd(dir, maxncpy);
#  endif
}
#endif /* !defined (__APPLE__) */

const char *BLI_dir_home()
{
  const char *home_dir;

  /* Return the users home directory with a fallback when the environment variable isn't set.
   * Failure to access `$HOME` is rare but possible, see: #2931.
   *
   * Any errors accessing home is likely caused by a broken/unsupported configuration,
   * nevertheless, failing to null check would crash which makes the error difficult
   * for users troubleshoot. */
  home_dir = BLI_getenv("HOME");
  if (home_dir == nullptr) {
    if (const passwd *pwuser = getpwuid(getuid())) {
      home_dir = pwuser->pw_dir;
    }
  }

  return home_dir;
}

double BLI_dir_free_space(const char *dir)
{
#ifdef USE_STATFS_STATVFS
  struct statvfs disk;
#else
  struct statfs disk;
#endif

  char dirname[FILE_MAXDIR], *slash;
  int len = strlen(dir);

  if (len >= FILE_MAXDIR) {
    /* path too long */
    return -1;
  }

  memcpy(dirname, dir, len + 1);

  if (len) {
    slash = strrchr(dirname, '/');
    if (slash) {
      slash[1] = '\0';
    }
  }
  else {
    dirname[0] = '/';
    dirname[1] = '\0';
  }

#  if defined(USE_STATFS_STATVFS)
  if (statvfs(dirname, &disk)) {
    return -1;
  }
#  elif defined(USE_STATFS_4ARGS)
  if (statfs(dirname, &disk, sizeof(struct statfs), 0)) {
    return -1;
  }
#  else
  if (statfs(dirname, &disk)) {
    return -1;
  }
#  endif

  return double(disk.f_bsize) * double(disk.f_bfree);
}

int64_t BLI_ftell(FILE *stream)
{
  return ftell(stream);
}

int BLI_fseek(FILE *stream, int64_t offset, int whence)
{
  return fseek(stream, offset, whence);
}

int64_t BLI_lseek(int fd, int64_t offset, int whence)
{
  return lseek(fd, offset, whence);
}

size_t BLI_file_descriptor_size(int file)
{
  BLI_stat_t st;
  if ((file < 0) || (BLI_fstat(file, &st) == -1)) {
    return -1;
  }
  return st.st_size;
}

size_t BLI_file_size(const char *path)
{
  BLI_stat_t stats;
  if (BLI_stat(path, &stats) == -1) {
    return -1;
  }
  return stats.st_size;
}

/* Return file attributes. Apple version of this function is defined in storage_apple.mm */
#ifndef __APPLE__
eFileAttributes BLI_file_attributes(const char *path)
{
  int ret = 0;

#  ifdef WIN32

  if (BLI_path_extension_check(path, ".lnk")) {
    return FILE_ATTR_ALIAS;
  }

  WCHAR wline[FILE_MAXDIR];
  if (conv_utf_8_to_16(path, wline, ARRAY_SIZE(wline)) != 0) {
    return eFileAttributes(ret);
  }

  DWORD attr = GetFileAttributesW(wline);
  if (attr == INVALID_FILE_ATTRIBUTES) {
    BLI_assert_msg(GetLastError() != ERROR_FILE_NOT_FOUND,
                   "BLI_file_attributes should only be called on existing files.");
    return eFileAttributes(ret);
  }

  if (attr & FILE_ATTRIBUTE_READONLY) {
    ret |= FILE_ATTR_READONLY;
  }
  if (attr & FILE_ATTRIBUTE_HIDDEN) {
    ret |= FILE_ATTR_HIDDEN;
  }
  if (attr & FILE_ATTRIBUTE_SYSTEM) {
    ret |= FILE_ATTR_SYSTEM;
  }
  if (attr & FILE_ATTRIBUTE_ARCHIVE) {
    ret |= FILE_ATTR_ARCHIVE;
  }
  if (attr & FILE_ATTRIBUTE_COMPRESSED) {
    ret |= FILE_ATTR_COMPRESSED;
  }
  if (attr & FILE_ATTRIBUTE_ENCRYPTED) {
    ret |= FILE_ATTR_ENCRYPTED;
  }
  if (attr & FILE_ATTRIBUTE_TEMPORARY) {
    ret |= FILE_ATTR_TEMPORARY;
  }
  if (attr & FILE_ATTRIBUTE_SPARSE_FILE) {
    ret |= FILE_ATTR_SPARSE_FILE;
  }
  if (attr & FILE_ATTRIBUTE_OFFLINE || attr & FILE_ATTRIBUTE_RECALL_ON_OPEN ||
      attr & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)
  {
    ret |= FILE_ATTR_OFFLINE;
  }
  if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
    ret |= FILE_ATTR_REPARSE_POINT;
  }

#  else

  UNUSED_VARS(path);

  /* TODO:
   * If Immutable set FILE_ATTR_READONLY
   * If Archived set FILE_ATTR_ARCHIVE
   */
#  endif
  return eFileAttributes(ret);
}
#endif

#ifndef __APPLE__ /* Apple version is defined in `storage_apple.mm`. */
bool BLI_file_alias_target(const char *filepath,
                           /* This parameter can only be `const` on Linux since
                            * redirection is not supported there.
                            * NOLINTNEXTLINE: readability-non-const-parameter. */
                           char r_targetpath[FILE_MAXDIR])
{
#  ifdef WIN32
  if (!BLI_path_extension_check(filepath, ".lnk")) {
    return false;
  }

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    return false;
  }

  IShellLinkW *Shortcut = nullptr;
  hr = CoCreateInstance(
      CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID *)&Shortcut);

  bool success = false;
  if (SUCCEEDED(hr)) {
    IPersistFile *PersistFile;
    hr = Shortcut->QueryInterface(IID_IPersistFile, (LPVOID *)&PersistFile);
    if (SUCCEEDED(hr)) {
      WCHAR path_utf16[FILE_MAXDIR] = {0};
      if (conv_utf_8_to_16(filepath, path_utf16, ARRAY_SIZE(path_utf16)) == 0) {
        hr = PersistFile->Load(path_utf16, STGM_READ);
        if (SUCCEEDED(hr)) {
          hr = Shortcut->Resolve(0, SLR_NO_UI | SLR_UPDATE | SLR_NOSEARCH);
          if (SUCCEEDED(hr)) {
            wchar_t target_utf16[FILE_MAXDIR] = {0};
            hr = Shortcut->GetPath(target_utf16, FILE_MAXDIR, nullptr, 0);
            if (SUCCEEDED(hr)) {
              success = (conv_utf_16_to_8(target_utf16, r_targetpath, FILE_MAXDIR) == 0);
            }
          }
          PersistFile->Release();
        }
      }
    }
    Shortcut->Release();
  }

  CoUninitialize();
  return (success && r_targetpath[0]);
#  else
  UNUSED_VARS(r_targetpath, filepath);
  /* File-based redirection not supported. */
  return false;
#  endif
}
#endif

int BLI_file_stat_mode(const char *path)
{
  struct stat st;
  BLI_assert(!BLI_path_is_rel(path));
  if (stat(path, &st)) {
    return 0;
  }
  return (st.st_mode);
}

bool BLI_exists(const char *path)
{
  return BLI_file_stat_mode(path) != 0;
}

int BLI_fstat(int fd, struct stat *buffer)
{
  return fstat(fd, buffer);
}

int BLI_stat(const char *path, struct stat *buffer)
{
  return stat(path, buffer);
}

bool BLI_is_dir(const char *path)
{
  return S_ISDIR(BLI_file_stat_mode(path));
}

bool BLI_is_file(const char *path)
{
  const int mode = BLI_file_stat_mode(path);
  return (mode && !S_ISDIR(mode));
}

void *BLI_file_read_data_as_mem_from_handle(FILE *fp,
                                            bool read_size_exact,
                                            size_t pad_bytes,
                                            size_t *r_size)
{
  /* NOTE: Used for both text and binary file reading. */

  BLI_stat_t st;
  if (BLI_fstat(fileno(fp), &st) == -1) {
    return nullptr;
  }
  if (S_ISDIR(st.st_mode)) {
    return nullptr;
  }
  if (BLI_fseek(fp, 0L, SEEK_END) == -1) {
    return nullptr;
  }
  /* Don't use the 'st_size' because it may be the symlink. */
  const long int filelen = BLI_ftell(fp);
  if (filelen == -1) {
    return nullptr;
  }
  if (BLI_fseek(fp, 0L, SEEK_SET) == -1) {
    return nullptr;
  }

  void *mem = MEM_new_uninitialized(filelen + pad_bytes, __func__);
  if (mem == nullptr) {
    return nullptr;
  }

  const long int filelen_read = fread(mem, 1, filelen, fp);
  if ((filelen_read < 0) || ferror(fp)) {
    MEM_delete_void(mem);
    return nullptr;
  }

  if (read_size_exact) {
    if (filelen_read != filelen) {
      MEM_delete_void(mem);
      return nullptr;
    }
  }
  else {
    if (filelen_read < filelen) {
      mem = MEM_realloc_uninitialized(mem, filelen_read + pad_bytes);
      if (mem == nullptr) {
        return nullptr;
      }
    }
  }

  *r_size = filelen_read;

  return mem;
}

char *BLI_file_read_text_as_mem(const char *filepath, size_t pad_bytes, size_t *r_size)
{
  FILE *fp = BLI_fopen(filepath, "r");
  char *mem = nullptr;
  if (fp) {
    mem = static_cast<char *>(BLI_file_read_data_as_mem_from_handle(fp, false, pad_bytes, r_size));
    fclose(fp);
  }
  return mem;
}

void *BLI_file_read_binary_as_mem(const char *filepath, size_t pad_bytes, size_t *r_size)
{
  FILE *fp = BLI_fopen(filepath, "rb");
  void *mem = nullptr;
  if (fp) {
    mem = BLI_file_read_data_as_mem_from_handle(fp, true, pad_bytes, r_size);
    fclose(fp);
  }
  return mem;
}

char *BLI_file_read_text_as_mem_with_newline_as_nil(const char *filepath,
                                                    bool trim_trailing_space,
                                                    size_t pad_bytes,
                                                    size_t *r_size)
{
  char *mem = BLI_file_read_text_as_mem(filepath, pad_bytes, r_size);
  if (mem != nullptr) {
    char *mem_end = mem + *r_size;
    if (pad_bytes != 0) {
      *mem_end = '\0';
    }
    for (char *p = mem, *p_next; p != mem_end; p = p_next) {
      p_next = static_cast<char *>(memchr(p, '\n', mem_end - p));
      if (p_next != nullptr) {
        if (trim_trailing_space) {
          for (char *p_trim = p_next - 1; p_trim > p && ELEM(*p_trim, ' ', '\t'); p_trim--) {
            *p_trim = '\0';
          }
        }
        *p_next = '\0';
        p_next++;
      }
      else {
        p_next = mem_end;
      }
    }
  }
  return mem;
}

LinkNode *BLI_file_read_as_lines(const char *filepath)
{
  FILE *fp = BLI_fopen(filepath, "r");
  LinkNodePair lines = {nullptr, nullptr};
  char *buf;
  size_t size;

  if (!fp) {
    return nullptr;
  }

  BLI_fseek(fp, 0, SEEK_END);
  size = size_t(BLI_ftell(fp));
  BLI_fseek(fp, 0, SEEK_SET);

  if (size == size_t(-1)) [[unlikely]] {
    fclose(fp);
    return nullptr;
  }

  buf = MEM_new_array_zeroed<char>(size, "file_as_lines");
  if (buf) {
    size_t i, last = 0;

    /*
     * size = because on win32 reading
     * all the bytes in the file will return
     * less bytes because of `CRNL` changes.
     */
    size = fread(buf, 1, size, fp);
    for (i = 0; i <= size; i++) {
      if (i == size || buf[i] == '\n') {
        char *line = BLI_strdupn(&buf[last], i - last);
        BLI_linklist_append(&lines, line);
        last = i + 1;
      }
    }

    MEM_delete(buf);
  }

  fclose(fp);

  return lines.list;
}

void BLI_file_free_lines(LinkNode *lines)
{
  BLI_linklist_freeN(lines);
}

bool BLI_file_older(const char *file1, const char *file2)
{
  BLI_stat_t st1, st2;
  if (BLI_stat(file1, &st1)) {
    return false;
  }
  if (BLI_stat(file2, &st2)) {
    return false;
  }
  return (st1.st_mtime < st2.st_mtime);
}

}  // namespace blender
