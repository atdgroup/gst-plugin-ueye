# Note: plugindir is set in configure

plugin_LTLIBRARIES = libueyeplugin.la

# Path to installation of the uEye SDK 
UEYE_CFLAGS = -I/usr/include
#UEYE_LIBS = -lueye_api.so.4.40 -L/usr/lib
UEYE_LIBS = -lueye_api -L/usr/lib

# sources used to compile this plug-in
libueyeplugin_la_SOURCES = gstueyesrc.c gstueyesrc.h gstplugin.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libueyeplugin_la_CFLAGS = $(GST_CFLAGS) $(UEYE_CFLAGS)
libueyeplugin_la_LIBADD = $(GST_LIBS) $(UEYE_LIBS) -lgstvideo-1.0
libueyeplugin_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libueyeplugin_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstueyesrc.h
