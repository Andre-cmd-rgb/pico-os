# Nofrendo, the NES emulator core. Someone else's code: see ../README.md,
# and COPYING.LIB here for its licence (Library GPL v2).
#
# The sources are only compiled when the emulator is switched on, but the
# component always registers: CONFIG_ values do not exist during CMake's
# first pass, so the include paths have to be unconditional.
set(srcs "")
set(fragments "")
if(CONFIG_PT_NES)
	set(fragments linker.lf)	# its static data in PSRAM
	file(GLOB mappers mappers/*.c)
	set(srcs
		nofrendo.c
		nes/apu.c
		nes/cpu.c
		nes/dis.c
		nes/input.c
		nes/mem.c
		nes/mmc.c
		nes/nes.c
		nes/ppu.c
		nes/rom.c
		nes/state.c
		${mappers})
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS . port LDFRAGMENTS ${fragments})

if(srcs)
	# RETRO_GO selects the hooks in port/. Its
	# memory is the nes program's own, counted as a program's is (pt/sys.h),
	# so a game killed from another terminal gives all of it back.
	target_compile_options(${COMPONENT_LIB} PRIVATE -w -DRETRO_GO)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE
		malloc=pt_malloc calloc=pt_calloc realloc=pt_realloc free=pt_free strdup=pt_strdup)
	# The interpreter runs in an ordinary NES task with caches enabled.
	# Its IRAM placement is for speed; ROM/memory accesses still need cache.
	# IDF disables switch tables globally for cache-off handlers, but that
	# turns this hot 256-opcode switch into a long tree of comparisons.
	set(nes_cpu_options "-fjump-tables")
	if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
		list(APPEND nes_cpu_options "-ftree-switch-conversion")
	endif()
	set_source_files_properties(nes/cpu.c PROPERTIES COMPILE_OPTIONS "${nes_cpu_options}")
endif()
