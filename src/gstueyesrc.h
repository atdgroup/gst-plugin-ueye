/* GStreamer uEye Plugin
 * Copyright (C) 2014 Gray Cancer Institute
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _GST_UEYE_SRC_H_
#define _GST_UEYE_SRC_H_

#include <gst/base/gstpushsrc.h>

#include  <ueye.h>

G_BEGIN_DECLS

#define GST_TYPE_UEYE_SRC   (gst_ueye_src_get_type())
#define GST_UEYE_SRC(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_UEYE_SRC,GstUEyeSrc))
#define GST_UEYE_SRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_UEYE_SRC,GstUEyeSrcClass))
#define GST_IS_UEYE_SRC(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_UEYE_SRC))
#define GST_IS_UEYE_SRC_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_UEYE_SRC))

typedef struct _GstUEyeSrc GstUEyeSrc;
typedef struct _GstUEyeSrcClass GstUEyeSrcClass;

typedef enum
{
	GST_WB_DISABLED,
	GST_WB_ONESHOT,
	GST_WB_AUTO
} WhiteBalanceType;

struct _GstUEyeSrc
{
  GstPushSrc base_ueye_src;

  // device
  HIDS hCam;  // device handle
  gboolean cameraPresent;
  SENSORINFO SensorInfo;  // device sensor information
  char *pcImgMem;  // pointer to the allocated image memory the device driver is using
  int lMemId;  // ID of the allocated memory
  INT nWidth;
  INT nHeight;
  INT nBitsPerPixel;
  INT nBytesPerPixel;
  INT nPitch;   // Stride in bytes between lines
  INT nImageSize;  // Image size in bytes

  gint gst_stride;  // Stride/pitch for the GStreamer buffer

  // gst properties
  gint pixelclock;
  gdouble exposure;
  gdouble framerate;
  gdouble maxframerate;
  gint gain;
  gint blacklevel;
  gint rgain;
  gint ggain;
  gint bgain;
  gint binning;
  gint vflip;
  gint hflip;
  WhiteBalanceType whitebalance;

  // stream
  gboolean acq_started;
  gint n_frames;
  gint total_timeouts;
  GstClockTime duration;
  GstClockTime last_frame_time;
};

struct _GstUEyeSrcClass
{
  GstPushSrcClass base_ueye_src_class;
};

GType gst_ueye_src_get_type (void);

G_END_DECLS

#endif
