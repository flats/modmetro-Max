cmake_minimum_required(VERSION 3.19)

# Build a universal (Apple Silicon + Intel) external unless told otherwise,
# e.g. -DCMAKE_OSX_ARCHITECTURES=arm64. Must be set before project().
if (APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
	set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures to build")
endif ()

project(modmetro C)

# Put the built modmetro~.mxo in ./externals instead of the Max package
# layout (../../../externals) that the SDK scripts assume by default.
set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/externals")

# max-pretarget.cmake names the project after this folder (modmetro-Max), so
# pin the output name. Max looks up the class by the bundle name, which must
# keep its tilde.
include(${CMAKE_CURRENT_SOURCE_DIR}/max-sdk-base/script/max-pretarget.cmake)
set(${PROJECT_NAME}_EXTERN_OUTPUT_NAME "modmetro~")

include_directories(
	"${MAX_SDK_INCLUDES}"
	"${MAX_SDK_MSP_INCLUDES}"
)

add_library(${PROJECT_NAME} MODULE
	modmetro.c
)

# Substituted into max-sdk-base/script/Info.plist.in
set(AUTHOR_DOMAIN "com.github.flats")
set(BUNDLE_IDENTIFIER "modmetro")
set(PACKAGE_VERSION "1.1.0")
set(COPYRIGHT_STRING "Copyright (c) 2016 D. Flaherty")
set(EXCLUDE_FROM_COLLECTIVES "no")

include(${CMAKE_CURRENT_SOURCE_DIR}/max-sdk-base/script/max-posttarget.cmake)
