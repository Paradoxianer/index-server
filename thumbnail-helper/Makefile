## Haiku Generic Makefile v2.6 ##

NAME = IndexServerThumbnailHelper
TYPE = APP
APP_MIME_SIG = application/x-vnd.Haiku-index_server_thumbnail_helper

SRCS = \
	ThumbnailHelper.cpp

RDEFS = ThumbnailHelper.rdef
RSRCS =

LIBS = be translation $(STDCPPLIBS)

LIBPATHS =

SYSTEM_INCLUDE_PATHS =

LOCAL_INCLUDE_PATHS =

OPTIMIZE :=
LOCALES =
DEFINES =
WARNINGS = ALL
SYMBOLS := TRUE
DEBUGGER := TRUE
COMPILER_FLAGS =
LINKER_FLAGS =
APP_VERSION :=
DRIVER_PATH =

# findpaths -r "makefile_engine" has proven unreliable on this VM (see
# server/Makefile's comment) - use the well-known system path directly.
include /boot/system/develop/etc/makefile-engine
