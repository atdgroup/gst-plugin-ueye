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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 *
 * Author P Barber
 */
/**
 * SECTION:element-gstueye_src
 *
 * The ueyesrc element is a source for a USB 3 camera supported by the IDS uEye SDK.
 * A live source, operating in push mode.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v ueyesrc ! autovideosink
 * ]|
 * </refsect2>
 */

// Which functions of the base class to override. Create must alloc and fill the buffer. Fill just needs to fill it
//#define OVERRIDE_FILL  !!! NOT IMPLEMENTED !!!
#define OVERRIDE_CREATE

#include <unistd.h> // for usleep
#include <string.h> // for memcpy

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#include "ueye.h"

#include "gstueyesrc.h"

GST_DEBUG_CATEGORY_STATIC (gst_ueye_src_debug);
#define GST_CAT_DEFAULT gst_ueye_src_debug

/* prototypes */
static void gst_ueye_src_set_property (GObject * object,
		guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_ueye_src_get_property (GObject * object,
		guint property_id, GValue * value, GParamSpec * pspec);
static void gst_ueye_src_dispose (GObject * object);
static void gst_ueye_src_finalize (GObject * object);

static gboolean gst_ueye_src_start (GstBaseSrc * src);
static gboolean gst_ueye_src_stop (GstBaseSrc * src);
static GstCaps *gst_ueye_src_get_caps (GstBaseSrc * src, GstCaps * filter);
static gboolean gst_ueye_src_set_caps (GstBaseSrc * src, GstCaps * caps);

#ifdef OVERRIDE_CREATE
	static GstFlowReturn gst_ueye_src_create (GstPushSrc * src, GstBuffer ** buf);
#endif
#ifdef OVERRIDE_FILL
	static GstFlowReturn gst_ueye_src_fill (GstPushSrc * src, GstBuffer * buf);
#endif

//static GstCaps *gst_ueye_src_create_caps (GstUEyeSrc * src);
static void gst_ueye_src_reset (GstUEyeSrc * src);
enum
{
	PROP_0,
	PROP_CAMERAPRESENT,
	PROP_EXPOSURE,
	PROP_PIXELCLOCK,
	PROP_GAIN,
	PROP_BLACKLEVEL,
	PROP_RGAIN,
	PROP_GGAIN,
	PROP_BGAIN,
	PROP_BINNING,
	PROP_HORIZ_FLIP,
	PROP_VERT_FLIP,
	PROP_WHITEBALANCE,
	PROP_MAXFRAMERATE
};


#define	UEYE_UPDATE_LOCAL  FALSE
#define	UEYE_UPDATE_CAMERA TRUE

#define DEFAULT_PROP_EXPOSURE           20.0
#define DEFAULT_PROP_PIXELCLOCK         50
#define DEFAULT_PROP_GAIN               0
#define DEFAULT_PROP_BLACKLEVEL         128
#define DEFAULT_PROP_RGAIN              0 //13   // Default values read from the uEye demo program
#define DEFAULT_PROP_GGAIN              0 // 0   // Default values read from the uEye demo program
#define DEFAULT_PROP_BGAIN              30  // 0 //18   // Default values read from the uEye demo program
#define DEFAULT_PROP_BINNING            1
#define DEFAULT_PROP_HORIZ_FLIP         0
#define DEFAULT_PROP_VERT_FLIP          0
#define DEFAULT_PROP_WHITEBALANCE       GST_WB_DISABLED
#define DEFAULT_PROP_MAXFRAMERATE       25

#define UEYE_REQUIRED_SYNC_PULSE_WIDTH 1   // in ms

#define DEFAULT_UEYE_VIDEO_FORMAT GST_VIDEO_FORMAT_BGR
// Put matching type text in the pad template below

// pad template
static GstStaticPadTemplate gst_ueye_src_template =
		GST_STATIC_PAD_TEMPLATE ("src",
				GST_PAD_SRC,
				GST_PAD_ALWAYS,
				GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
						("{ BGR }"))
		);

// error check, use in functions where 'src' is declared and initialised
#define UEYEEXECANDCHECK(function)\
{\
	INT Ret = function;\
	if (IS_SUCCESS!=Ret){\
		IS_CHAR*  pcErr=NULL;\
		INT Err=0;\
		is_GetError (src->hCam, &Err, &pcErr);\
		GST_ERROR_OBJECT(src, "uEye call failed with: %s", pcErr);\
	}\
}
// I removed this line because it caused a glib error when dynamically changing the pipeline to start recording.
// But no "uEye call failed ..." error is seen, with or without it!
// g_free(pcErr);\    // This free was in the UEYEEXECANDCHECK define, after the GST_ERROR_OBJECT line.

#define TYPE_WHITEBALANCE (whitebalance_get_type ())
static GType
whitebalance_get_type (void)
{
  static GType whitebalance_type = 0;

  if (!whitebalance_type) {
    static GEnumValue wb_types[] = {
	  { GST_WB_DISABLED, "Auto white balance disabled.",    "disabled" },
	  { GST_WB_ONESHOT,  "One shot white balance.", "oneshot"  },
	  { GST_WB_AUTO, "Auto white balance.", "auto" },
      { 0, NULL, NULL },
    };

    whitebalance_type =
	g_enum_register_static ("WhiteBalanceType", wb_types);
  }

  return whitebalance_type;
}

static void
gst_ueye_set_camera_exposure (GstUEyeSrc * src, gboolean send)
{  // How should the pipeline be told/respond to a change in frame rate - seems to be ok with a push source
	src->framerate = 1000.0/(src->exposure + UEYE_REQUIRED_SYNC_PULSE_WIDTH); // set a suitable frame rate for the exposure, if too fast for usb camera it will slow down, but add ms to exposure so there is always an output pulse in the deadtime created between frames.
	src->framerate = MIN(src->framerate, src->maxframerate);
	src->duration = 1000000000.0/src->framerate;  // frame duration in ns
	if (send){
		GST_DEBUG_OBJECT(src, "Request frame rate to %.1f, duration %d us, and exposure to %.1f ms", src->framerate, GST_TIME_AS_USECONDS(src->duration), src->exposure);
		is_SetFrameRate(src->hCam, src->framerate, &src->framerate); // set a suitable frame rate for the exposure, if too fast for usb camera will slow it down, get the actual frame rate back
		is_Exposure(src->hCam, IS_EXPOSURE_CMD_SET_EXPOSURE, (void*)&(src->exposure), sizeof(src->exposure));
		// Get the exposure value actually set back from the camera
		is_Exposure(src->hCam, IS_EXPOSURE_CMD_GET_EXPOSURE, (void*)&(src->exposure), sizeof(src->exposure));
		// Update the duration to the actual value
		src->duration = 1000000000.0/src->framerate;  // frame duration in ns
		GST_DEBUG_OBJECT(src, "Set frame rate to %.1f, duration %d us, and exposure to %.1f ms", src->framerate, GST_TIME_AS_USECONDS(src->duration), src->exposure);
	}
}

static void
gst_ueye_set_camera_binning (GstUEyeSrc * src)
{
	if(src->binning==2){
		is_SetBinning(src->hCam, IS_BINNING_2X_VERTICAL);
		is_SetBinning(src->hCam, IS_BINNING_2X_HORIZONTAL);
	}
	else{
		is_SetBinning(src->hCam, IS_BINNING_DISABLE);
	}
}

static void
gst_ueye_set_camera_whitebalance (GstUEyeSrc * src)
{
	switch (src->whitebalance){
		double dblAutoWb;
	case GST_WB_AUTO:   // the following code is from the uEye demo program (tabProcessing.cpp)
		dblAutoWb = 0.0;
		is_SetAutoParameter (src->hCam, IS_SET_AUTO_WB_ONCE, &dblAutoWb, NULL);
		dblAutoWb = 1.0;
		is_SetAutoParameter (src->hCam, IS_SET_ENABLE_AUTO_WHITEBALANCE, &dblAutoWb, NULL);
		break;
	case GST_WB_ONESHOT:
		dblAutoWb = 1.0;
		is_SetAutoParameter (src->hCam, IS_SET_AUTO_WB_ONCE, &dblAutoWb, NULL);
		is_SetAutoParameter (src->hCam, IS_SET_ENABLE_AUTO_WHITEBALANCE, &dblAutoWb, NULL);
		// TODO somehow, after the WB finished signal is received, return state to GST_WB_DISABLED. Seems OK without this.
		break;
	case GST_WB_DISABLED:
	default:
		dblAutoWb = 0.0;
		is_SetAutoParameter (src->hCam, IS_SET_AUTO_WB_ONCE, &dblAutoWb, NULL);
		is_SetAutoParameter (src->hCam, IS_SET_ENABLE_AUTO_WHITEBALANCE, &dblAutoWb, NULL);
		break;
	}
}

/* class initialisation */

G_DEFINE_TYPE (GstUEyeSrc, gst_ueye_src, GST_TYPE_PUSH_SRC);

static void
gst_ueye_src_class_init (GstUEyeSrcClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
	GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
	GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "ueyesrc", 0,
			"uEye Camera source");

	gobject_class->set_property = gst_ueye_src_set_property;
	gobject_class->get_property = gst_ueye_src_get_property;
	gobject_class->dispose = gst_ueye_src_dispose;
	gobject_class->finalize = gst_ueye_src_finalize;

	gst_element_class_add_pad_template (gstelement_class,
			gst_static_pad_template_get (&gst_ueye_src_template));

	gst_element_class_set_static_metadata (gstelement_class,
			"uEye Video Source", "Source/Video",
			"uEye Camera video source", "Paul R. Barber <paul.barber@oncology.ox.ac.uk>");

	gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_ueye_src_start);
	gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_ueye_src_stop);
	gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_ueye_src_get_caps);
	gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_ueye_src_set_caps);

#ifdef OVERRIDE_CREATE
	gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_ueye_src_create);
	GST_DEBUG ("Using gst_ueye_src_create.");
#endif
#ifdef OVERRIDE_FILL
	gstpushsrc_class->fill   = GST_DEBUG_FUNCPTR (gst_ueye_src_fill);
	GST_DEBUG ("Using gst_ueye_src_fill.");
#endif

	// Install GObject properties
	// Camera Present property
	g_object_class_install_property (gobject_class, PROP_CAMERAPRESENT,
			g_param_spec_boolean ("devicepresent", "Camera Device Present", "Is the camera present and connected OK?",
					FALSE, G_PARAM_READABLE));
	// Pixel CLock property
	g_object_class_install_property (gobject_class, PROP_PIXELCLOCK,
	  g_param_spec_int("pixelclock", "Pixel Clock", "Camera sensor pixel clock (MHz).", 7, 86, DEFAULT_PROP_PIXELCLOCK,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// Exposure property
	g_object_class_install_property (gobject_class, PROP_EXPOSURE,
	  g_param_spec_double("exposure", "Exposure", "Camera sensor exposure time (ms).", 0.01, 2000, DEFAULT_PROP_EXPOSURE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// Gain property
	g_object_class_install_property (gobject_class, PROP_GAIN,
	  g_param_spec_int("gain", "Gain", "Camera sensor master gain.", 0, 100, DEFAULT_PROP_GAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// Black Level property
	g_object_class_install_property (gobject_class, PROP_BLACKLEVEL,
	  g_param_spec_int("blacklevel", "Black Level", "Camera sensor black level offset.", 0, 255, DEFAULT_PROP_BLACKLEVEL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// R gain property
	g_object_class_install_property (gobject_class, PROP_RGAIN,
	  g_param_spec_int("rgain", "Red Gain", "Camera sensor red channel gain.", 0, 100, DEFAULT_PROP_RGAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// G gain property
	g_object_class_install_property (gobject_class, PROP_GGAIN,
	  g_param_spec_int("ggain", "Green Gain", "Camera sensor green channel gain.", 0, 100, DEFAULT_PROP_GGAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// B gain property
	g_object_class_install_property (gobject_class, PROP_BGAIN,
	  g_param_spec_int("bgain", "Blue Gain", "Camera sensor blue channel gain.", 0, 100, DEFAULT_PROP_BGAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// binning property, cannot be changed on the fly as image size changes
	g_object_class_install_property (gobject_class, PROP_BINNING,
	  g_param_spec_int("binning", "Binning", "Camera sensor binning. Not sure this works - FIXME!", 1, 2, DEFAULT_PROP_BINNING,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));
	// vflip property
	g_object_class_install_property (gobject_class, PROP_VERT_FLIP,
	  g_param_spec_int("vflip", "Vertical flip", "Image up-down flip.", 0, 1, DEFAULT_PROP_HORIZ_FLIP,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// hflip property
	g_object_class_install_property (gobject_class, PROP_HORIZ_FLIP,
	  g_param_spec_int("hflip", "Horizontal flip", "Image left-right flip.", 0, 1, DEFAULT_PROP_VERT_FLIP,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// White balance property
	g_object_class_install_property (gobject_class, PROP_WHITEBALANCE,
	  g_param_spec_enum("whitebalance", "White Balance", "White Balance mode. Disabled, One Shot or Auto. Not sure this works - FIXME!", TYPE_WHITEBALANCE, DEFAULT_PROP_WHITEBALANCE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// Max Frame Rate property
	g_object_class_install_property (gobject_class, PROP_MAXFRAMERATE,
	  g_param_spec_double("maxframerate", "Maximum Frame Rate", "Camera sensor maximum allowed frame rate (fps)."
			  "The frame rate will be determined from the exposure time, up to this maximum value when short exposures are used", 10, 200, DEFAULT_PROP_MAXFRAMERATE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
}

static void
gst_ueye_src_init (GstUEyeSrc * src)
{
	/* set source as live (no preroll) */
	gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

	/* override default of BYTES to operate in time mode */
	gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

	// Initialise properties
	src->exposure = DEFAULT_PROP_EXPOSURE;
	src->pixelclock = DEFAULT_PROP_PIXELCLOCK;
	gst_ueye_set_camera_exposure(src, UEYE_UPDATE_LOCAL);
	src->gain = DEFAULT_PROP_GAIN;
	src->blacklevel = DEFAULT_PROP_BLACKLEVEL;
	src->rgain = DEFAULT_PROP_RGAIN;
	src->ggain = DEFAULT_PROP_GGAIN;
	src->bgain = DEFAULT_PROP_BGAIN;
	src->binning = DEFAULT_PROP_BINNING;
	src->vflip = DEFAULT_PROP_VERT_FLIP;
	src->hflip = DEFAULT_PROP_HORIZ_FLIP;
	src->whitebalance = DEFAULT_PROP_WHITEBALANCE;
	src->maxframerate = DEFAULT_PROP_MAXFRAMERATE;

	gst_ueye_src_reset (src);
}

static void
gst_ueye_src_reset (GstUEyeSrc * src)
{
	src->hCam=0;
	src->cameraPresent = FALSE;
	src->n_frames=0;
	src->total_timeouts = 0;
	src->last_frame_time = 0;
}

void
gst_ueye_src_set_property (GObject * object, guint property_id,
		const GValue * value, GParamSpec * pspec)
{
	GstUEyeSrc *src;

	src = GST_UEYE_SRC (object);

	switch (property_id) {
	case PROP_CAMERAPRESENT:
		src->cameraPresent = g_value_get_boolean (value);
		break;
	case PROP_PIXELCLOCK:
		src->pixelclock = g_value_get_int (value);
		is_PixelClock(src->hCam, IS_PIXELCLOCK_CMD_SET, (void*)&(src->pixelclock), sizeof(src->pixelclock));
		break;
	case PROP_EXPOSURE:
		src->exposure = g_value_get_double(value);
		gst_ueye_set_camera_exposure(src, UEYE_UPDATE_CAMERA);
		break;
	case PROP_GAIN:
		src->gain = g_value_get_int (value);
		is_SetHardwareGain(src->hCam, src->gain, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
		break;
	case PROP_BLACKLEVEL:
		src->blacklevel = g_value_get_int (value);
		is_Blacklevel(src->hCam, IS_BLACKLEVEL_CMD_SET_OFFSET, (void*)&(src->blacklevel), sizeof(src->blacklevel));
		break;
	case PROP_RGAIN:
		src->rgain = g_value_get_int (value);
		is_SetHardwareGain(src->hCam, IS_IGNORE_PARAMETER, src->rgain, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
		break;
	case PROP_GGAIN:
		src->ggain = g_value_get_int (value);
		is_SetHardwareGain(src->hCam, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, src->ggain, IS_IGNORE_PARAMETER);
		break;
	case PROP_BGAIN:
		src->bgain = g_value_get_int (value);
		is_SetHardwareGain(src->hCam, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, src->bgain);
		break;
	case PROP_BINNING:
		src->binning = g_value_get_int (value);
		gst_ueye_set_camera_binning(src);
		break;
	case PROP_HORIZ_FLIP:
		src->hflip = g_value_get_int (value);
		is_SetRopEffect(src->hCam, IS_SET_ROP_MIRROR_LEFTRIGHT, src->hflip, 0);
		break;
	case PROP_VERT_FLIP:
		src->vflip = g_value_get_int (value);
		is_SetRopEffect(src->hCam, IS_SET_ROP_MIRROR_UPDOWN, src->vflip, 0);
		break;
	case PROP_WHITEBALANCE:
		src->whitebalance = g_value_get_enum (value);
		gst_ueye_set_camera_whitebalance(src);
		break;
	case PROP_MAXFRAMERATE:
		src->maxframerate = g_value_get_double(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

void
gst_ueye_src_get_property (GObject * object, guint property_id,
		GValue * value, GParamSpec * pspec)
{
	GstUEyeSrc *src;

	g_return_if_fail (GST_IS_UEYE_SRC (object));
	src = GST_UEYE_SRC (object);

	switch (property_id) {
	case PROP_CAMERAPRESENT:
		g_value_set_boolean (value, src->cameraPresent);
		break;
	case PROP_PIXELCLOCK:
		is_PixelClock(src->hCam, IS_PIXELCLOCK_CMD_GET, (void*)&(src->pixelclock), sizeof(src->pixelclock));
		g_value_set_int (value, src->pixelclock);
		break;
	case PROP_EXPOSURE:
		is_Exposure(src->hCam, IS_EXPOSURE_CMD_GET_EXPOSURE, (void*)&(src->exposure), sizeof(src->exposure));
		g_value_set_double (value, src->exposure);
		break;
	case PROP_GAIN:
		src->gain = is_SetHardwareGain(src->hCam, IS_GET_MASTER_GAIN, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
		g_value_set_int (value, src->gain);
		break;
	case PROP_BLACKLEVEL:
		is_Blacklevel(src->hCam, IS_BLACKLEVEL_CMD_GET_OFFSET, (void*)&(src->blacklevel), sizeof(src->blacklevel));
		g_value_set_int (value, src->blacklevel);
		break;
	case PROP_RGAIN:
		src->rgain = is_SetHardwareGain(src->hCam, IS_GET_RED_GAIN, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
		g_value_set_int (value, src->rgain);
		break;
	case PROP_GGAIN:
		src->ggain = is_SetHardwareGain(src->hCam, IS_GET_GREEN_GAIN, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
		g_value_set_int (value, src->ggain);
		break;
	case PROP_BGAIN:
		src->bgain = is_SetHardwareGain(src->hCam, IS_GET_BLUE_GAIN, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
		g_value_set_int (value, src->bgain);
		break;
	case PROP_BINNING:
		g_value_set_int (value, src->binning);
		break;
	case PROP_HORIZ_FLIP:
		g_value_set_int (value, src->hflip);
		break;
	case PROP_VERT_FLIP:
		g_value_set_int (value, src->vflip);
		break;
	case PROP_WHITEBALANCE:
		g_value_set_enum (value, src->whitebalance);
		break;
	case PROP_MAXFRAMERATE:
		g_value_set_double (value, src->maxframerate);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

void
gst_ueye_src_dispose (GObject * object)
{
	GstUEyeSrc *src;

	g_return_if_fail (GST_IS_UEYE_SRC (object));
	src = GST_UEYE_SRC (object);

	GST_DEBUG_OBJECT (src, "dispose");

	// clean up as possible.  may be called multiple times

	G_OBJECT_CLASS (gst_ueye_src_parent_class)->dispose (object);
}

void
gst_ueye_src_finalize (GObject * object)
{
	GstUEyeSrc *src;

	g_return_if_fail (GST_IS_UEYE_SRC (object));
	src = GST_UEYE_SRC (object);

	GST_DEBUG_OBJECT (src, "finalize");

	/* clean up object here */
	G_OBJECT_CLASS (gst_ueye_src_parent_class)->finalize (object);
}

static gboolean
gst_ueye_src_start (GstBaseSrc * bsrc)
{
	// Start will open the device but not start it, set_caps starts it, stop should stop and close it (as v4l2src)

	GstUEyeSrc *src = GST_UEYE_SRC (bsrc);

	GST_DEBUG_OBJECT (src, "start");

	// Turn on automatic timestamping, if so we do not need to do it manually, BUT there is some evidence that automatic timestamping is laggy
//	gst_base_src_set_do_timestamp(bsrc, TRUE);

	// read libversion (for informational purposes only)
	int version = is_GetDLLVersion();
	int build = version & 0xFFFF;
	version = version >> 16;
	int minor = version & 0xFF;
	version = version >> 8;
	int major = version & 0xFF;
	GST_INFO_OBJECT (src, "uEye Library Ver %d.%d.%d", major, minor, build);


	// open first usable device
	GST_DEBUG_OBJECT (src, "is_InitCamera");
	src->hCam=0;
	UEYEEXECANDCHECK(is_InitCamera(&(src->hCam), NULL));

	// display error when no camera has been found
	if(!src->hCam)
	{
		GST_ERROR_OBJECT (src, "No uEye device found.");
		goto fail;
	}

	// NOTE:
	// from now on, the "hCam" handle can be used to access the camera board.
	// use is_ExitCamera to end the usage
	src->cameraPresent = TRUE;

	// Enable the receiving of frame events (we wait for these in create)
	GST_DEBUG_OBJECT (src, "is_EnableEvent");
	UEYEEXECANDCHECK(is_EnableEvent(src->hCam, IS_SET_EVENT_FRAME_RECEIVED));
	// TODO what is IS_SET_EVENT_FIRST_PACKET_RECEIVED?

	// Get information about the camera sensor
	GST_DEBUG_OBJECT (src, "is_GetSensorInfo");
	UEYEEXECANDCHECK(is_GetSensorInfo(src->hCam, &(src->SensorInfo)));

	// We will use the the full sensor
	src->nWidth = src->SensorInfo.nMaxWidth;
	src->nHeight = src->SensorInfo.nMaxHeight;

	// Colour format
	//is_SetColorMode() can tell us the color mode
	//src->pSensorInfo->nColorMode reports on a different color 'mode'
	// We support just colour of one type, BGR 24-bit, I am not attempting to support all camera types
	src->nBitsPerPixel = 24;

	// Alloc some buffers
	GST_DEBUG_OBJECT (src, "is_AllocImageMem");
	UEYEEXECANDCHECK(is_AllocImageMem(src->hCam, src->nWidth, src->nHeight, src->nBitsPerPixel, &(src->pcImgMem), &(src->lMemId)));

	// from uEyePixelPeekDlg.cpp demo code
	GST_DEBUG_OBJECT (src, "is_SetImageMem");
	UEYEEXECANDCHECK(is_SetImageMem(src->hCam, src->pcImgMem, src->lMemId));
	GST_DEBUG_OBJECT (src, "is_InquireImageMem");
	is_InquireImageMem(src->hCam, src->pcImgMem, src->lMemId, &(src->nWidth), &(src->nHeight), &(src->nBitsPerPixel), &(src->nPitch));
	src->nBytesPerPixel = (src->nBitsPerPixel+1)/8;
	src->nImageSize = src->nWidth * src->nHeight * src->nBytesPerPixel;
	GST_DEBUG_OBJECT (src, "Image is %d x %d, pitch %d, bpp %d, Bpp %d", src->nWidth, src->nHeight, src->nPitch, src->nBitsPerPixel, src->nBytesPerPixel);

	is_PixelClock(src->hCam, IS_PIXELCLOCK_CMD_SET, (void*)&(src->pixelclock), sizeof(src->pixelclock));

	//is_SetHardwareGamma(src->hCam, IS_SET_HW_GAMMA_ON);  // Hardware gamma is rubbish at the low intensity range
	// set software gamma to some value, times value by 100 and send to camera, i.e. for 1.8 send 180
	{
		INT nGamma=180;
		is_Gamma(src->hCam, IS_GAMMA_CMD_SET, (void*)&nGamma, sizeof(nGamma));
	}

	// turn on the output 'flash' sync pulse direct from the camera
	{
		UINT nMode;
		IO_FLASH_PARAMS flashParams;
		UINT nValue;

		GST_DEBUG_OBJECT (src, "Setting flash trigger. is_IO()");

		nMode = IO_FLASH_MODE_FREERUN_HI_ACTIVE;
		UEYEEXECANDCHECK(is_IO(src->hCam, IS_IO_CMD_FLASH_SET_MODE, (void*)&nMode, sizeof(nMode)));

		flashParams.s32Delay = 0;
		flashParams.u32Duration = 0; // 5000; // us, or 0=exposure time
		UEYEEXECANDCHECK(is_IO(src->hCam, IS_IO_CMD_FLASH_SET_PARAMS, (void*)&flashParams, sizeof(flashParams)));

		// Enable flash auto freerun
		nValue = IS_FLASH_AUTO_FREERUN_ON;
		UEYEEXECANDCHECK(is_IO(src->hCam, IS_IO_CMD_FLASH_SET_AUTO_FREERUN, (void*)&nValue, sizeof(nValue)));
	}

	gst_ueye_set_camera_exposure(src, UEYE_UPDATE_CAMERA);
	is_SetHardwareGain(src->hCam, src->gain, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
	is_Blacklevel(src->hCam, IS_BLACKLEVEL_CMD_SET_OFFSET, (void*)&(src->blacklevel), sizeof(src->blacklevel));
	is_SetHardwareGain(src->hCam, IS_IGNORE_PARAMETER, src->rgain, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER);
	is_SetHardwareGain(src->hCam, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, src->ggain, IS_IGNORE_PARAMETER);
	is_SetHardwareGain(src->hCam, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, IS_IGNORE_PARAMETER, src->bgain);
	gst_ueye_set_camera_binning(src);
	is_SetRopEffect(src->hCam, IS_SET_ROP_MIRROR_LEFTRIGHT, src->hflip, 0);
	is_SetRopEffect(src->hCam, IS_SET_ROP_MIRROR_UPDOWN, src->vflip, 0);
	gst_ueye_set_camera_whitebalance(src);

	return TRUE;

	fail:
	if (src->hCam) {
		is_ExitCamera(src->hCam);
		src->hCam = 0;
	}

	return FALSE;
}

static gboolean
gst_ueye_src_stop (GstBaseSrc * bsrc)
{
	// Start will open the device but not start it, set_caps starts it, stop should stop and close it (as v4l2src)

	GstUEyeSrc *src = GST_UEYE_SRC (bsrc);

	GST_DEBUG_OBJECT (src, "stop");
	UEYEEXECANDCHECK(is_StopLiveVideo(src->hCam, IS_FORCE_VIDEO_STOP));
	UEYEEXECANDCHECK(is_DisableEvent(src->hCam, IS_SET_EVENT_FRAME_RECEIVED));
	UEYEEXECANDCHECK(is_ExitCamera(src->hCam));

	gst_ueye_src_reset (src);

	return TRUE;
}

static GstCaps *
gst_ueye_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
	GstUEyeSrc *src = GST_UEYE_SRC (bsrc);
	GstCaps *caps;

  if (src->hCam == 0) {
    caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (src));
  } else {
    GstVideoInfo vinfo;

    // Create video info 
    gst_video_info_init (&vinfo);

    vinfo.width = src->nWidth;
    vinfo.height = src->nHeight;

   	vinfo.fps_n = 0;  vinfo.fps_d = 1;  // Frames per second fraction n/d, 0/1 indicates a frame rate may vary
    vinfo.interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

    vinfo.finfo = gst_video_format_get_info (DEFAULT_UEYE_VIDEO_FORMAT);

    // cannot do this for variable frame rate
    //src->duration = gst_util_uint64_scale_int (GST_SECOND, vinfo.fps_d, vinfo.fps_n); // NB n and d are wrong way round to invert the fps into a duration.

    caps = gst_video_info_to_caps (&vinfo);
  }

	GST_DEBUG_OBJECT (src, "The caps are %" GST_PTR_FORMAT, caps);

	if (filter) {
		GstCaps *tmp = gst_caps_intersect (caps, filter);
		gst_caps_unref (caps);
		caps = tmp;

		GST_DEBUG_OBJECT (src, "The caps after filtering are %" GST_PTR_FORMAT, caps);
	}

	return caps;
}

static gboolean
gst_ueye_src_set_caps (GstBaseSrc * bsrc, GstCaps * caps)
{
	// Start will open the device but not start it, set_caps starts it, stop should stop and close it (as v4l2src)

	GstUEyeSrc *src = GST_UEYE_SRC (bsrc);
	GstVideoInfo vinfo;
	//GstStructure *s = gst_caps_get_structure (caps, 0);

	GST_DEBUG_OBJECT (src, "The caps being set are %" GST_PTR_FORMAT, caps);

	gst_video_info_from_caps (&vinfo, caps);

	if (GST_VIDEO_INFO_FORMAT (&vinfo) != GST_VIDEO_FORMAT_UNKNOWN) {
		g_assert (src->hCam != 0);
		//  src->vrm_stride = get_pitch (src->device);  // wait for image to arrive for this
		src->gst_stride = GST_VIDEO_INFO_COMP_STRIDE (&vinfo, 0);
		src->nHeight = vinfo.height;
	} else {
		goto unsupported_caps;
	}

	// start freerun/continuous capture

	UEYEEXECANDCHECK(is_CaptureVideo(src->hCam, IS_FORCE_VIDEO_START));
	src->acq_started = TRUE;

	return TRUE;

	unsupported_caps:
	GST_ERROR_OBJECT (src, "Unsupported caps: %" GST_PTR_FORMAT, caps);
	return FALSE;
}

//  This can override the push class create fn, it is the same as fill above but it forces the creation of a buffer here to copy into.
#ifdef OVERRIDE_CREATE
static GstFlowReturn
gst_ueye_src_create (GstPushSrc * psrc, GstBuffer ** buf)
{
	GstUEyeSrc *src = GST_UEYE_SRC (psrc);
	GstMapInfo minfo;

	// lock next (raw) image for read access, convert it to the desired
	// format and unlock it again, so that grabbing can go on

	// Wait for the next image to be ready
	INT timeout = 5000.0/src->framerate;  // 5 times the frame period in ms
	INT nRet = is_WaitEvent(src->hCam, IS_SET_EVENT_FRAME_RECEIVED, timeout);

	if(G_LIKELY(nRet == IS_SUCCESS))
	{
		//  successfully returned an image
		// ----------------------------------------------------------

		guint i;

		// Copy image to buffer in the right way

		// Create a new buffer for the image
		*buf = gst_buffer_new_and_alloc (src->nHeight * src->gst_stride);

		gst_buffer_map (*buf, &minfo, GST_MAP_WRITE);

		// From the grabber source we get 1 progressive frame
		// We expect src->vrm_stride = src->gst_stride but use separate vars for safety

		for (i = 0; i < src->nHeight; i++) {
			memcpy (minfo.data + i * src->gst_stride,
					src->pcImgMem + i * src->nPitch, src->nPitch);
		}

		gst_buffer_unmap (*buf, &minfo);

		// If we do not use gst_base_src_set_do_timestamp() we need to add timestamps manually
		src->last_frame_time += src->duration;   // Get the timestamp for this frame
		if(!gst_base_src_get_do_timestamp(GST_BASE_SRC(psrc))){
			GST_BUFFER_PTS(*buf) = src->last_frame_time;  // convert ms to ns
			GST_BUFFER_DTS(*buf) = src->last_frame_time;  // convert ms to ns
		}
		GST_BUFFER_DURATION(*buf) = src->duration;
//		GST_DEBUG_OBJECT(src, "pts, dts: %" GST_TIME_FORMAT ", duration: %d ms", GST_TIME_ARGS (src->last_frame_time), GST_TIME_AS_MSECONDS(src->duration));

		// count frames, and send EOS when required frame number is reached
		GST_BUFFER_OFFSET(*buf) = src->n_frames;  // from videotestsrc
		src->n_frames++;
		GST_BUFFER_OFFSET_END(*buf) = src->n_frames;  // from videotestsrc
		if (psrc->parent.num_buffers>0)  // If we were asked for a specific number of buffers, stop when complete
			if (G_UNLIKELY(src->n_frames >= psrc->parent.num_buffers))
				return GST_FLOW_EOS;

		// see, if we had to drop some frames due to data transfer stalls. if so,
		// output a message
	}
	else
	{
		// did not return an image. why?
		// ----------------------------------------------------------
		switch(nRet)
		{
		case IS_TIMED_OUT:
			GST_ERROR_OBJECT(src, "is_WaitEvent() timed out.");
			break;
		default:
			GST_ERROR_OBJECT(src, "is_WaitEvent() failed with a generic error.");
			break;
		}
		return GST_FLOW_ERROR;
	}

	return GST_FLOW_OK;
}
#endif // OVERRIDE_CREATE

// Override the push class fill fn, using the default create and alloc fns.
// buf is the buffer to fill, it may be allocated in alloc or from a downstream element.
// Other functions such as deinterlace do not work with this type of buffer.
#ifdef OVERRIDE_FILL
static GstFlowReturn
gst_ueye_src_fill (GstPushSrc * psrc, GstBuffer * buf_external)
{
	GstUEyeSrc *src = GST_UEYE_SRC (psrc);
	GstMapInfo minfo;
	guint8 *image;
	VRmImage* p_source_img=0;
	VRmDWORD frames_dropped=0;
	VRmBOOL ready=FALSE;
	GstBuffer ** buf = &buf_external;

	// lock next (raw) image for read access, convert it to the desired
	// format and unlock it again, so that grabbing can go on

	// Wait for the next image to be ready
	time_t start_time = time(NULL);
	do {
		VRmUsbCamIsNextImageReadyEx(src->device, src->port, &ready);
		usleep(src->image_poll_time_us);
	}
	while(!ready && (time(NULL)-start_time)<5);  // wait for frame ready or 5 sec timeout

	if(G_LIKELY(VRmUsbCamLockNextImageEx2(src->device, src->port, &p_source_img, &frames_dropped, 5000)))
	{
		// VRmUsbCamLockNextImageEx2() successfully returned an image
		// ----------------------------------------------------------

		// Get a pointer to the image buffer
		image = p_source_img->mp_buffer;

		// Return the pitch, between lines of the image, in bytes.
		src->vrm_stride = p_source_img->m_pitch;

		guint i, j;

		// Copy image to buffer in the right way

	    switch (src->avc_readout){
	    case UEYE_PROPID_GRAB_AVC_READOUT_FRAME:

			gst_buffer_map (*buf, &minfo, GST_MAP_WRITE);

			// From the grabber source we get 2 fields combined, interleave these into the target
			// i steps through the source line by line, j through the target every other line
			// use 2 loops, one for each field
			// We expect src->vrm_stride = src->gst_stride but use separate vars for safety
			for (i = 0, j=0; i < src->height/2; i++, j+=2) {
				memcpy (minfo.data + j * src->gst_stride,
						image + i * src->vrm_stride, src->vrm_stride);
			}
			for (i = src->height/2, j=1; i < src->height; i++, j+=2) {
				memcpy (minfo.data + j * src->gst_stride,
						image + i * src->vrm_stride, src->vrm_stride);
			}

			gst_buffer_unmap (*buf, &minfo);

			break;
	    case UEYE_PROPID_GRAB_AVC_READOUT_FIELD:
			if (src->interlace){
				// From the grabber source we get field by field, copy these into the local buffer
				// i steps through the source line by line, j through the target every other line
				// We expect src->vrm_stride = src->gst_stride but use separate vars for safety

				gst_buffer_map (src->local_buffer, &minfo, GST_MAP_WRITE);

				// Check m_image_modifier bits for the field identifier
				// Start on the first or second row based on this
				j = p_source_img->m_image_format.m_image_modifier & UEYE_INTERLACED_FIELD0 ? 0 : 1;

				for (i = 0; i < src->height; i++, j+=2) {
					memcpy (minfo.data + j * src->gst_stride,
							image + i * src->vrm_stride, src->vrm_stride);
				}

				// Copy the local buffer to the frame buffer
				gst_buffer_fill(*buf, 0, minfo.data, minfo.size);

				gst_buffer_unmap (src->local_buffer, &minfo);
			}
			else{

				gst_buffer_map (*buf, &minfo, GST_MAP_WRITE);

				// From the grabber source we get field by field, copy these into the target
				// i steps through the source line by line, j through the target
				// We expect src->vrm_stride = src->gst_stride but use separate vars for safety
				for (i = 0; i < src->height; i++) {
					memcpy (minfo.data + i * src->gst_stride,
							image + i * src->vrm_stride, src->vrm_stride);
				}

				gst_buffer_unmap (*buf, &minfo);
			}

	    	break;
	    default:
	    	GST_WARNING_OBJECT(src, "Invalid AVC Readout property value");
	    	break;
	    }


		// We are leaving the buffer metadata timestamps DTS and PTS and the duration as GST_CLOCK_TIME_NONE
		// indicating they are unknown/undefined
//		GST_FIXME_OBJECT(src, "time: %f", p_source_img->m_time_stamp);
//		GST_BUFFER_PTS(*buf) = (GstClockTime)(p_source_img->m_time_stamp * 1000000.0);  // convert ms to ns
//		GST_BUFFER_DTS(*buf) = (GstClockTime)(p_source_img->m_time_stamp * 1000000.0);  // convert ms to ns
		GST_BUFFER_DURATION (*buf) = src->duration;

		VRmUsbCamUnlockNextImage(src->device, &p_source_img);

		// count frames, and send EOS when required frame number is reached
		GST_BUFFER_OFFSET(*buf) = src->n_frames;  // from videotestsrc
		src->n_frames++;
		GST_BUFFER_OFFSET_END(*buf) = src->n_frames;  // from videotestsrc
		if (psrc->parent.num_buffers>0)  // If we were asked for a specific number of buffers, stop when complete
			if (G_UNLIKELY(src->n_frames >= psrc->parent.num_buffers))
				return GST_FLOW_EOS;

		// We are leaving the buffer metadata timestamps DTS and PTS and the duration as GST_CLOCK_TIME_NONE
		// indicating they are unknown/undefined

		// see, if we had to drop some frames due to data transfer stalls. if so,
		// output a message
		if (G_UNLIKELY(frames_dropped)) {
			GST_INFO_OBJECT (src, "Received timeout, %d dropped frames", frames_dropped);
			src->total_timeouts += frames_dropped;
		}
	}
	else
	{
		// VRmUsbCamLockNextImageEx2() did not return an image. why?
		// ----------------------------------------------------------
		switch(VRmUsbCamGetLastErrorCode())
		{
		case UEYE_ERROR_CODE_FUNCTION_CALL_TIMEOUT:
		case UEYE_ERROR_CODE_TRIGGER_TIMEOUT:
		case UEYE_ERROR_CODE_TRIGGER_STALL:
			GST_ERROR_OBJECT(src, "VRmUsbCamLockNextImageEx2() failed with %s", VRmUsbCamGetLastError());
			break;
		case UEYE_ERROR_CODE_GENERIC_ERROR:
		default:
			GST_ERROR_OBJECT(src, "VRmUsbCamLockNextImageEx2() failed with a generic error.");
			break;
		}
		return GST_FLOW_ERROR;
	}

	return GST_FLOW_OK;
}
#endif // OVERRIDE_FILL

