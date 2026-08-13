## Haiku Generic Makefile v2.6 ##

NAME = MailAnalyser
TYPE = SHARED
APP_MIME_SIG =

SRCS = \
	MailAnalyser.cpp \
	../shared/CLuceneDataBase.cpp \
	../shared/RunWithTimeout.cpp \
	../IndexServerAddOn.cpp

RDEFS =
RSRCS =

LIBS = be translation mail clucene-core clucene-shared $(STDCPPLIBS)

LIBPATHS =

SYSTEM_INCLUDE_PATHS = \
	../../headers

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

DEVEL_DIRECTORY := \
	$(shell findpaths -r "makefile_engine" B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine
