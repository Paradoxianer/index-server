## Haiku Generic Makefile v2.6 ##

NAME = FullTextAnalyser
TYPE = SHARED
APP_MIME_SIG =

SRCS = \
	../shared/CLuceneDataBase.cpp \
	FullTextAnalyser.cpp \
	../shared/RunWithTimeout.cpp \
	../IndexServerAddOn.cpp

RDEFS =
RSRCS =

LIBS = be translation clucene-core clucene-shared $(STDCPPLIBS)

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

# findpaths -r "makefile_engine" has proven unreliable on this VM (see
# server/Makefile's comment) - use the well-known system path directly.
include /boot/system/develop/etc/makefile-engine
