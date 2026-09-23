idf_component_register(
	SRCS
		auth.c
		clock.c
		devfs.c
		file.c
		keys.c
		klog.c
		match.c
		mem.c
		mount.c
		path.c
		pipe.c
		proc.c
		procfs.c
		sys.c
	INCLUDE_DIRS include
	REQUIRES freertos log
	PRIV_REQUIRES esp_app_format esp_hw_support esp_psram esp_timer heap mbedtls spi_flash vfs
)
