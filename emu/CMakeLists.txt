# The port layer: screen, sound and joypad for the emulator cores.
set(srcs "")
if(CONFIG_PT_NES)
	set(srcs nes_port.c)
endif()

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS .
	REQUIRES kernel drivers nofrendo
	PRIV_REQUIRES esp_timer
)

if(srcs)
	# The core's headers pick their host with this, ours included.
	target_compile_options(${COMPONENT_LIB} PRIVATE -DRETRO_GO)
endif()
