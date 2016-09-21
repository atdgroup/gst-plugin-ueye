#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstueyesrc.h"

#define GST_CAT_DEFAULT gst_gstueye_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "ueyesrc", 0,
      "debug category for uEye elements");

  if (!gst_element_register (plugin, "ueyesrc", GST_RANK_NONE,
          GST_TYPE_UEYE_SRC)) {
    return FALSE;
  }

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    ueye,
    "uEye camera source element.",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, "http://users.ox.ac.uk/~atdgroup")
