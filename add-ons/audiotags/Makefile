## Haiku Generic Makefile v2.6 ##

NAME = AudioTagAnalyser
TYPE = SHARED
APP_MIME_SIG =

SRCS = \
	AudioTagAnalyser.cpp \
	../shared/RunWithTimeout.cpp \
	../IndexServerAddOn.cpp

RDEFS =
RSRCS =

LIBS = be tag $(STDCPPLIBS)

LIBPATHS =

SYSTEM_INCLUDE_PATHS = \
	../../headers \
	/boot/system/develop/headers/taglib

LOCAL_INCLUDE_PATHS = \
	../shared

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
