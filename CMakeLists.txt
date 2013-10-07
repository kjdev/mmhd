# CMake version
CMAKE_MINIMUM_REQUIRED(VERSION 2.8 FATAL_ERROR)

# Build type
SET(CMAKE_BUILD_TYPE Debug)
SET(CMAKE_C_FLAGS_RELEASE "-Wall -O2")
SET(CMAKE_C_FLAGS_DEBUG "-W -g")

# Project name
# mmhd: micro markdown http server
PROJECT(mmhd)
SET(MAJOR_VERSION 1)
SET(MINOR_VERSION 1)
SET(BUILD_VERSION 0)
#SET(REVISION_VERSION 0)

# Configure
CONFIGURE_FILE(
  ${PROJECT_SOURCE_DIR}/src/config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/config.h)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

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

# hoedown sources
SET(HOEDOWN_SOURCES
  src/hoedown/src/autolink.c src/hoedown/src/buffer.c src/hoedown/src/escape.c
  src/hoedown/src/html.c src/hoedown/src/html_blocks.c
  src/hoedown/src/html_smartypants.c src/hoedown/src/markdown.c
  src/hoedown/src/stack.c)

# execute
ADD_EXECUTABLE(mmhd src/main.c ${HOEDOWN_SOURCES})
TARGET_LINK_LIBRARIES(mmhd ${LIBMICROHTTPD_LIBS})

# include
INSTALL_PROGRAMS(/bin FILES
  ${CMAKE_CURRENT_BINARY_DIR}/mmhd)
