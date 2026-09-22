# The MP3 decoder. Someone else's code: see ../README.md.
idf_component_register(
	SRCS decoders.c
	INCLUDE_DIRS .
)

# Built as it comes, without the tree's warning flags.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
