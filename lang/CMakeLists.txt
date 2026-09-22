idf_component_register(
	SRCS
		builtins.c
		compile.c
		driver.c
		image.c
		lex.c
		port_pt.c
		quicken.c
		runtime.c
		vm.c
	INCLUDE_DIRS .
	REQUIRES kernel
	WHOLE_ARCHIVE
)

# The dispatch loop is the only hot code here: -O3 is measurably faster and,
# unlike the compiler, does not grow much (about 9 KB either way).
set_source_files_properties(vm.c PROPERTIES COMPILE_OPTIONS "-O3")
