cmake_minimum_required(VERSION 3.22)

# The tree is laid out like a kernel source tree: each top-level directory
# below is an ESP-IDF component.
set(EXTRA_COMPONENT_DIRS kernel drivers shell bin lang codec emu third_party)
# Applications kept outside this tree (make APPS="DIR ..."): each directory
# an ESP-IDF component, built in as if it were here. Kept in the build's
# cache, so later builds in the same directory include them too.
if(PT_EXTRA_APPS)
	list(APPEND EXTRA_COMPONENT_DIRS ${PT_EXTRA_APPS})
endif()
# The same as PT_VERSION (kernel/include/pt/kernel.h), for the image's header
set(PROJECT_VER "1.0-beta5")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One lock file per chip: the ESP32-P4 pulls in Wi-Fi components the S3
# does not (drivers/idf_component.yml), and a shared file would be
# rewritten by every build for the other chip.
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
# With applications from outside, their dependencies are locked in the build
# directory instead, starting from this tree's: its own file stays as it is.
if(PT_EXTRA_APPS)
	set(pt_lock ${CMAKE_BINARY_DIR}/dependencies.lock.${IDF_TARGET})
	if(NOT EXISTS ${pt_lock})
		configure_file(dependencies.lock.${IDF_TARGET} ${pt_lock} COPYONLY)
	endif()
	idf_build_set_property(DEPENDENCIES_LOCK ${pt_lock})
endif()
project(pico-os)
