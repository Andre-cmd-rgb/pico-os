idf_component_register(
	SRCS
		audio/audio.c
		audio/es8311.c
		input/blepad.c
		input/button.c
		input/cardkb.c
		input/serial.c
		input/usbkbd.c
		misc/i2c.c
		misc/usbhost.c
		net/modem.c
		net/netconsole.c
		net/wifi.c
		misc/led.c
		power/battery.c
		power/cpufreq.c
		power/suspend.c
		storage/ramdisk.c
		storage/rootfs.c
		storage/sdcard.c
		storage/usbmsc.c
		tty/tty.c
		tty/vt.c
		video/capture.c
		video/ili9341.c
		video/io_i80.c
		video/io_spi.c
	INCLUDE_DIRS include
	PRIV_INCLUDE_DIRS tty
	REQUIRES kernel ulp
	PRIV_REQUIRES
		esp_adc
		esp_driver_gpio
		esp_driver_i2c
		esp_driver_i2s
		esp_driver_ledc
		esp_driver_pcnt
		esp_driver_rmt
		esp_driver_sdspi
		esp_driver_spi
		esp_driver_uart
		esp_driver_usb_serial_jtag
		esp_hw_support
		esp_pm
		esp_event
		esp_lcd
		esp_netif
		lwip
		bt
		esp_hid
		esp_wifi
		nvs_flash
		esp_timer
		fatfs
		heap
		sdmmc
		vfs
		wear_levelling
)

# The program the low-power RISC-V core runs during suspend.
if(CONFIG_ULP_COPROC_TYPE_RISCV)
	set(ulp_app_name ulp_${COMPONENT_NAME})
	ulp_embed_binary(${ulp_app_name} "power/ulp/wake.c" "power/suspend.c")
endif()
