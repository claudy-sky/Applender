/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdio.h>

namespace blender {

/** \file
 * \ingroup bli
 */

int BLI_cpu_support_sse2();
int BLI_cpu_support_sse42();
/**
 * Write a backtrace into a file for systems which support it.
 */
void BLI_system_backtrace_with_os_info(FILE *fp, const void *os_info);
void BLI_system_backtrace(FILE *fp);

/** Get CPU brand, result is to be MEM_delete()-ed. */
char *BLI_cpu_brand_string();

/**
 * Obtain the hostname from the system.
 *
 * This simply determines the host's name, and doesn't do any DNS lookup of any
 * IP address of the machine. As such, it's only usable for identification
 * purposes, and not for reachability over a network.
 *
 * \param buffer: Character buffer to write the hostname into.
 * \param buffer_maxncpy: Size of the character buffer, including trailing '\0'.
 */
void BLI_hostname_get(char *buffer, size_t buffer_maxncpy);

/** Get maximum addressable memory in megabytes. */
size_t BLI_system_memory_max_in_megabytes();
/** Get maximum addressable memory in megabytes (clamped to #INT_MAX). */
int BLI_system_memory_max_in_megabytes_int();

/**
 * Ensure the process can open many files simultaneously.
 * This should be called once on application startup, as it is not thread safe.
 */
void BLI_system_max_open_files_ensure();

/* For `getpid`. */
#define BLI_SYSTEM_PID_H <unistd.h>

}  // namespace blender
