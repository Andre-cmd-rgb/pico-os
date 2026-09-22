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
	# Built as it comes, with RETRO_GO selecting the hooks in port/.
	target_compile_options(${COMPONENT_LIB} PRIVATE -w -DRETRO_GO)
endif()
