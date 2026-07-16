/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2024-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#pragma once

#include <cstdint>

#include "IMB_imbuf_enums.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct SwsContext;

#ifdef WITH_FFMPEG

extern "C" {
#  include <libavutil/rational.h>
}

#endif

namespace blender {

struct IDProperty;

struct MovieReader {
  enum class State { Uninitialized, Failed, Valid };
  ImBufFlags ib_flags = ImBufFlags::Zero;
  State state = State::Uninitialized;
  int cur_position = 0; /* index  0 = 1e,  1 = 2e, enz. */
  int duration_in_frames = 0;
  int frs_sec = 0;
  double frs_sec_base = 0.0;
  double start_offset = 0.0;
  int x = 0;
  int y = 0;
  int video_rotation = 0;

  /* for number */
  char filepath[/*FILE_MAX*/ 1024] = {};

  int streamindex = 0;

  bool keep_original_colorspace = false;

#ifdef WITH_FFMPEG
  AVFormatContext *pFormatCtx = nullptr;
  AVCodecContext *pCodecCtx = nullptr;
  const AVCodec *pCodec = nullptr;
  AVFrame *pFrameRGB = nullptr;
  AVFrame *pFrameDeinterlaced = nullptr;
  SwsContext *img_convert_ctx = nullptr;
  /* Conversion context for frames downloaded from a hardware decoder: their
   * pixel format (e.g. NV12) differs from the stream's software format that
   * `img_convert_ctx` was created for. */
  SwsContext *img_convert_ctx_hw = nullptr;
  /* AVPixelFormat of downloaded hardware frames, AV_PIX_FMT_NONE (-1) when unused. */
  int hwdec_transfer_format = -1;
  /* Hardware (VideoToolbox) decoding is set up and did not fail so far. */
  bool hwdec_active = false;
  int videoStream = 0;

  AVFrame *pFrame = nullptr;
  bool pFrame_complete = false;
  AVFrame *pFrame_backup = nullptr;
  bool pFrame_backup_complete = false;

  int64_t cur_pts = 0;
  int64_t cur_key_frame_pts = 0;
  AVPacket *cur_packet = nullptr;

  AVRational frame_rate = {1, 1};

  bool seek_before_decode = false;
  bool is_float = false;

  /* When set, never seek within the video, and only ever decode one frame.
   * This is a workaround for some Ogg files that have full audio but only
   * one frame of "album art" as a video stream in non-Theora format.
   * ffmpeg crashes/aborts when trying to seek within them
   * (https://trac.ffmpeg.org/ticket/10755). */
  bool never_seek_decode_one_frame = false;
#endif

  char proxy_dir[768] = {};

  int proxies_tried = 0;

  MovieReader *proxy_anim[IMB_PROXY_MAX_SLOT] = {};

  char colorspace[/*MAX_COLORSPACE_NAME*/ 64] = {};
  /** The maximum name from multi-view. */
  char suffix[/*MAX_NAME*/ 64] = {};

  IDProperty *metadata = nullptr;
};

}  // namespace blender
