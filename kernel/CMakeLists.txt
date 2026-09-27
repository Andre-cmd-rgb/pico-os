idf_component_register(
	SRCS
		auth.c
		clock.c
		devfs.c
		file.c
		internal.c
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

# Every flash call goes through internal.c, which moves it off a process's
# PSRAM stack.
foreach(fn esp_flash_read esp_flash_write esp_flash_erase_region esp_flash_erase_chip
	   esp_flash_read_encrypted esp_flash_write_encrypted)
	target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
endforeach()
