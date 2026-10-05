# The decoders' cost on the chip's own instruction set, under QEMU: see
# tools/perfbench.py, which makes the media, builds this and runs it.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(perfbench)
