# The real sources, from the tree two levels up; the media and the cut-out
# 2:1 filter come from tools/perfbench.py (PB_GEN).
set(pico ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(
	SRCS perfbench.c
		${pico}/codec/codec.c ${pico}/codec/flac.c ${pico}/codec/mp3.c ${pico}/codec/wav.c
		${pico}/third_party/minimp3/decoders.c
		${pico}/bin/jpeg.c
	INCLUDE_DIRS . ${PB_GEN} ${pico}/codec ${pico}/third_party/minimp3 ${pico}/kernel/include
		${pico}/bin ${pico}/drivers/audio
	PRIV_REQUIRES esp_psram esp_rom
	EMBED_FILES ${PB_GEN}/media.bin
)
set_source_files_properties(${pico}/third_party/minimp3/decoders.c PROPERTIES COMPILE_OPTIONS -w)
