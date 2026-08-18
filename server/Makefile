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
# findpaths -r "makefile_engine" (the standard template's own approach)
# depends on a resolvable-name package lookup that has proven unreliable
# on this VM (observed failing outright after heavy package/build
# activity, for reasons not fully understood) - the plain well-known
# system path is what that lookup resolves to anyway on a standard
# install, so use it directly instead of routing through a lookup that
# can spuriously fail.
include /boot/system/develop/etc/makefile-engine
