# The port layer: screen, sound and joypad for the emulator cores.
set(srcs "")
set(fragments "")
if(CONFIG_PT_NES)
	set(srcs nes_port.c)
	set(fragments linker.lf)	# its static data in PSRAM
endif()

idf_component_register(
	SRCS ${srcs}
	LDFRAGMENTS ${fragments}
	INCLUDE_DIRS .
	REQUIRES kernel drivers nofrendo
	PRIV_REQUIRES esp_timer
)

if(srcs)
	# The core's headers pick their host with this, ours included.
	target_compile_options(${COMPONENT_LIB} PRIVATE -DRETRO_GO)
endif()
