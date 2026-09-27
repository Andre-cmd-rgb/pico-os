idf_component_register(
	SRCS
		arith.c
		builtins.c
		complete.c
		exec.c
		expand.c
		jobs.c
		lineedit.c
		parse.c
		sh.c
		test.c
	REQUIRES kernel
	WHOLE_ARCHIVE
)
