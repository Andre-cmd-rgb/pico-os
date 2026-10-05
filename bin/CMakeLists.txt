# The commands. The system's own are always built; the applications are
# each a menuconfig option (pico-os > Applications), and what is left out
# can come from pico-os-packages instead.
set(srcs
	audio.c
	cal.c
	dates.c
	diff.c
	edit.c
	fileutils.c
	filters.c
	find.c
	http.c
	man.c
	net.c
	passwd.c
	pick.c
	pkg.c
	regex.c
	sed.c
	setup.c
	shellutils.c
	sink.c
	sort.c
	sums.c
	sysutils.c
	textutils.c
	theme.c
	top.c
	util.c)
set(embed
	doc/intro.txt
	doc/commands.txt
	doc/pico.txt
	doc/hardware.txt)

if(CONFIG_PT_APP_AI)
	list(APPEND srcs ai.c ai_jobs.c ai_models.c ai_path.c ai_session.c ai_stats.c ai_ui.c json.c)
	list(APPEND embed ai/study.txt ai/code.txt ai/search.txt)
endif()
if(CONFIG_PT_APP_NOTES)
	list(APPEND srcs notes.c)
endif()
if(CONFIG_PT_APP_DIARY)
	list(APPEND srcs alarm.c calendar.c todo.c)
endif()
if(CONFIG_PT_APP_CALC)
	list(APPEND srcs calc.c)
endif()
if(CONFIG_PT_APP_PICTURES)
	list(APPEND srcs canvas.c jpeg.c video.c view.c)
endif()
if(CONFIG_PT_APP_MUSIC)
	list(APPEND srcs music.c tags.c lrc.c jpeg.c)
endif()
if(CONFIG_PT_NES)
	list(APPEND srcs nes.c)
endif()
if(CONFIG_PT_APP_BENCH)
	list(APPEND srcs bench.c)
endif()
if(CONFIG_PT_APP_FETCH)
	list(APPEND srcs fetch.c)
endif()
if(CONFIG_PT_BLE)
	list(APPEND srcs ble.c)
endif()

list(REMOVE_DUPLICATES srcs)	# jpeg.c: pictures and music both

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS .
	REQUIRES kernel drivers codec emu
	PRIV_REQUIRES esp_app_format esp_driver_gpio esp_hw_support esp_netif esp_system esp_timer heap lwip mbedtls esp_http_client esp-tls
	WHOLE_ARCHIVE
	EMBED_TXTFILES ${embed}
)
