# Nofrendo, the NES emulator core. Someone else's code: see ../README.md,
# and COPYING here for its licence (GPL v2).
#
# The sources are only compiled when the emulator is switched on, but the
# component always registers: CONFIG_ values do not exist during CMake's
# first pass, so the include paths have to be unconditional.
set(srcs "")
if(CONFIG_PT_NES)
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

idf_component_register(SRCS ${srcs} INCLUDE_DIRS . port)

if(srcs)
	# Built as it comes, with RETRO_GO selecting the hooks in port/. Its
	# memory is the nes program's own, counted as a program's is (pt/sys.h),
	# so a game killed from another terminal gives all of it back.
	target_compile_options(${COMPONENT_LIB} PRIVATE -w -DRETRO_GO)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE
		malloc=pt_malloc calloc=pt_calloc realloc=pt_realloc free=pt_free strdup=pt_strdup)
endif()
