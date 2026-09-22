idf_component_register(
	SRCS
		audio.c
		bench.c
		canvas.c
		edit.c
		fileutils.c
		man.c
		nes.c
		net.c
		pick.c
		sysutils.c
		textutils.c
		top.c
		util.c
		video.c
		view.c
		xfer.c
	REQUIRES kernel drivers codec emu
	PRIV_REQUIRES esp_app_format esp_driver_gpio esp_hw_support esp_netif esp_system esp_timer heap lwip
	WHOLE_ARCHIVE
	EMBED_TXTFILES
		doc/intro.txt
		doc/commands.txt
		doc/language.txt
		doc/hardware.txt
)
