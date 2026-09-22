cmake_minimum_required(VERSION 3.22)

# The tree is laid out like a kernel source tree: each top-level directory
# below is an ESP-IDF component.
set(EXTRA_COMPONENT_DIRS kernel drivers shell bin lang codec emu third_party)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pockettype)
