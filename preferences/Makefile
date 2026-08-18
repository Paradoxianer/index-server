## Haiku Generic Makefile v2.6 ##

NAME = IndexServerSettings
TYPE = APP
APP_MIME_SIG = application/x-vnd.Haiku-IndexServerSettings

SRCS = \
	IndexServerPreflet.cpp \
	SettingsWindow.cpp \
	SettingsBackend.cpp

RDEFS = IndexServerSettings.rdef
RSRCS =

LIBS = be tracker localestub columnlistview $(STDCPPLIBS)

LIBPATHS =

SYSTEM_INCLUDE_PATHS = \
	../headers \
	../server \
	/boot/system/develop/headers/private/interface

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
