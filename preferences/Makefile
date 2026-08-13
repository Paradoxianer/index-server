## Haiku Generic Makefile v2.6 ##

NAME = IndexServerSettings
TYPE = APP
APP_MIME_SIG = application/x-vnd.Haiku-IndexServerSettings

SRCS = \
	IndexServerPreflet.cpp \
	SettingsWindow.cpp \
	../server/IndexServerSettings.cpp

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

DEVEL_DIRECTORY := \
	$(shell findpaths -r "makefile_engine" B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine
