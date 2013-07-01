# CMake version
CMAKE_MINIMUM_REQUIRED(VERSION 2.8 FATAL_ERROR)

# Build type
SET(CMAKE_BUILD_TYPE Debug)
SET(CMAKE_C_FLAGS_RELEASE "-Wall -O2")
SET(CMAKE_C_FLAGS_DEBUG "-W -g")

# Project name
# mmhd: micro markdown http server
PROJECT(mmhd)
# Project version
# SET(_VERSION "1.0.0")

# libmicrohttpd
FIND_PATH(LIBMICROHTTPD_INCLUDES
  microhttpd.h
  PATHS ${LIBMICROHTTPD_INCLUDE_PATH} /usr/include)

FIND_LIBRARY(LIBMICROHTTPD_LIBRARIES
  NAMES libmicrohttpd microhttpd
  PATHS ${LIBMICROHTTPD_LIBRARY_PATH} /usr/lib64 /usr/lib)

IF(LIBMICROHTTPD_INCLUDES STREQUAL "LIBMICROHTTPD_INCLUDES-NOTFOUND")
  MESSAGE(FATAL_ERROR "libmicrohttpd could not found microhttpd.h\n"
    "OPTION: -DLIBMICROHTTPD_INCLUDE_PATH=path")
ENDIF()

IF(LIBMICROHTTPD_LIBRARIES STREQUAL "LIBMICROHTTPD_LIBRARIES-NOTFOUND")
  MESSAGE(FATAL_ERROR "libmicrohttpd could not found libmicrohttpd.so\n"
    "OPTION: -DLIBMICROHTTPD_LIBRARY_PATH=path")
ENDIF()

INCLUDE_DIRECTORIES(${LIBMICROHTTPD_INCLUDES})
SET(LIBMICROHTTPD_LIBS "${LIBMICROHTTPD_LIBRARIES}")

# sundown sources
SET(SUNDOWN_SOURCES
  src/sundown/autolink.c src/sundown/houdini_html_e.c src/sundown/markdown.c
  src/sundown/buffer.c src/sundown/html.c src/sundown/stack.c
  src/sundown/houdini_href_e.c src/sundown/html_smartypants.c)

# execute
ADD_EXECUTABLE(mmhd src/main.c ${SUNDOWN_SOURCES})
TARGET_LINK_LIBRARIES(mmhd ${LIBMICROHTTPD_LIBS})

# include
INSTALL_PROGRAMS(/bin FILES
  ${CMAKE_CURRENT_BINARY_DIR}/mmhd)
