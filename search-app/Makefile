## Haiku Generic Makefile v2.6 ##

NAME = IndexServerSearch
TYPE = APP
APP_MIME_SIG = application/x-vnd.Haiku-IndexServerSearch

SRCS = \
	IndexSearchApp.cpp \
	SearchWindow.cpp

RDEFS = IndexServerSearch.rdef
RSRCS =

LIBS = be localestub columnlistview $(STDCPPLIBS)

LIBPATHS =

SYSTEM_INCLUDE_PATHS = \
	../headers \
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
