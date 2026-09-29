idf_component_register(
	SRCS
		codec.c
		flac.c
		mp3.c
		voice.c
		wav.c
	INCLUDE_DIRS .
	REQUIRES kernel minimp3
)
