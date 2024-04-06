cmake_minimum_required(VERSION 3.15)
project(rlawt C)

set(CMAKE_C_STANDARD 11)

find_package(Java 1.8 REQUIRED)
include(UseJava)
set(CMAKE_FIND_FRAMEWORK NEVER) # does not find jni otherwise
find_package(JNI REQUIRED)
unset(CMAKE_FIND_FRAMEWORK)

set(CMAKE_SKIP_RPATH TRUE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9")

set(CMAKE_JAVA_COMPILE_FLAGS "-source" "1.8" "-target" "1.8")
add_jar(jar AWTContext.java GENERATE_NATIVE_HEADERS rlawt-headers)

if (MSVC)
	add_compile_options(/W4)
else()
	add_compile_options(-Wall)
endif()

add_library(rlawt SHARED rlawt.c rlawt_nix.c rlawt_windows.c)

target_link_libraries(rlawt rlawt-headers ${JNI_LIBRARIES})

if (WIN32)
	include_directories(include/windows)
	set_property(TARGET rlawt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded") # multi-threaded statically-linked runtime
	target_link_libraries(rlawt opengl32)
elseif (APPLE)
	target_sources(rlawt PRIVATE rlawt_mac.m)
	find_library(CORE_FOUNDATION CoreFoundation)
	find_library(QUARTZ_CORE QuartzCore)
	find_library(IO_SURFACE IOSurface)
	find_library(OPENGL OpenGL)
	find_library(APPKIT AppKit)
	set_property(SOURCE rlawt.c APPEND PROPERTY COMPILE_OPTIONS -x objective-c)
	target_link_libraries(rlawt ${CORE_FOUNDATION} ${QUARTZ_CORE} ${IO_SURFACE} ${OPENGL} ${APPKIT})
elseif (UNIX)
	target_link_libraries(rlawt GL GLX)
endif ()
