/* SPDX-FileCopyrightText: 2016 by Mike Erwin. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#define PROGRAM_NO_OPTI 0
// #define GPU_NO_USE_PY_REFERENCES

/* GPU_INLINE */
#define GPU_INLINE static inline __attribute__((always_inline)) __attribute__((__unused__))
