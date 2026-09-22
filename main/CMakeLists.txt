idf_component_register(
	SRCS init.c memtest.c
	PRIV_REQUIRES kernel drivers esp_app_format esp_hw_support esp_mm esp_timer heap vfs
)
