cmake_minimum_required(VERSION 3.22)

# The tree is laid out like a kernel source tree: each top-level directory
# below is an ESP-IDF component.
set(EXTRA_COMPONENT_DIRS kernel drivers shell bin lang codec emu third_party)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One lock file per chip: the ESP32-P4 pulls in Wi-Fi components the S3
# does not (drivers/idf_component.yml), and a shared file would be
# rewritten by every build for the other chip.
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(pockettype)
