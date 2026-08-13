## Haiku Generic Makefile v2.6 ##

NAME = index_server
TYPE = APP
APP_MIME_SIG = application/x-vnd.Haiku-index_server

SRCS = \
	CatchUpManager.cpp \
	IndexProgressNotifier.cpp \
	main.cpp \
	IndexServer.cpp \
	IndexServerSettings.cpp \
	VolumeWatcher.cpp \
	../add-ons/IndexServerAddOn.cpp \
	../vendor/storage-kit/AddOnMonitorHandler.cpp \
	../vendor/storage-kit/NodeMonitorHandler.cpp

RDEFS = index_server.rdef

RSRCS =

LIBS = be localestub $(STDCPPLIBS)

LIBPATHS =

SYSTEM_INCLUDE_PATHS = \
	../headers \
	../vendor/storage-kit \
	/boot/system/develop/headers/private/system \
	/boot/system/develop/headers/private/system/arch/x86_64

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

## Include the Makefile-Engine
DEVEL_DIRECTORY := \
	$(shell findpaths -r "makefile_engine" B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine
