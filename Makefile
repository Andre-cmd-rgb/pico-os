# Linux-style front end for idf.py:
#
#   make defconfig     start this board's configuration from its board file
#   make menuconfig    configure drivers, pins and features
#   make               build
#   make flash         flash the board
#   make term          serial console (Ctrl-] to quit)
#   make flash term    both
#   make test          PC tests, then the QEMU image with the device suites
#   make hosttest      the pico compiler and VM, the JPEG decoder, the text programs
#   make hwtest        the shell suite on the board at PORT
#   make progtest      the text programs on the board, against GNU's on this PC
#   make scripttest    shell control flow (if/for/while/case, functions)
#   make langtest      the pico language on the device
#   make push FILE=... [DEST=...]   copy a file to the board (default ~/name)
#   make pull FILE=... [DEST=...]   copy a file from the board
#   make video FILE=film.mkv        convert a video into clips/, send it if small
#   make stress        hammer the board at PORT for SECONDS (default 600)
#
# BOARD picks which drivers and pins to start from (boards/*.defconfig, and
# boards/README.md); each board builds in its own directory, build/BOARD:
#
#   make BOARD=devkit-uno-shield flash term
#
# FRAGMENTS adds boards/fragments/NAME.config on top of the board file,
# like the kernel's config fragments; they seed a configuration the way
# the board file does, so they go with defconfig:
#
#   make BOARD=freenove-fnk0104b FRAGMENTS=ble defconfig
#
# PORT defaults to the first /dev/ttyUSB* or /dev/ttyACM* found.

# ESP-IDF's export.sh finds its own directory under bash, not under the
# dash that is /bin/sh on Debian and Ubuntu.
SHELL    := bash

BOARD    ?= freenove-fnk0104b
FRAGMENTS ?=
BUILD    ?= build/$(BOARD)
IDF_PATH ?= $(HOME)/esp/esp-idf
export IDF_PATH
PORT     ?= $(firstword $(wildcard /dev/ttyUSB*) $(wildcard /dev/ttyACM*))
EXPORT   := . $(IDF_PATH)/export.sh >/dev/null
DEFAULTS := sdkconfig.defaults;boards/$(BOARD).defconfig$(subst $(eval) ,,$(foreach f,$(FRAGMENTS),;boards/fragments/$(f).config))
IDF      := $(EXPORT) && idf.py -B $(BUILD) -D SDKCONFIG=$(BUILD)/sdkconfig \
	    -D "SDKCONFIG_DEFAULTS=$(DEFAULTS)"

.PHONY: all build defconfig menuconfig flash time term monitor clean distclean size font boards stale-config \
	test hosttest hwtest progtest scripttest langtest push pull video stress need-port need-board

all: build

# The build date `uname` and the boot log show is compiled into ESP-IDF's
# app description, which is otherwise only rebuilt when IDF changes: so
# every build compiles it again, and the date says which firmware this is.
APP_DESC := $(BUILD)/esp-idf/esp_app_format/CMakeFiles/__idf_esp_app_format.dir/esp_app_desc.c.obj

build: need-board stale-config
	@rm -f $(APP_DESC)
	@$(IDF) build

# The board file and fragments only seed a configuration that does not
# exist yet, so changing them later is silent unless someone says so.
stale-config:
	@if [ -f $(BUILD)/sdkconfig ] && \
	    [ boards/$(BOARD).defconfig -nt $(BUILD)/sdkconfig ]; then \
		echo "note: boards/$(BOARD).defconfig is newer than $(BUILD)/sdkconfig;"; \
		echo "      run 'make BOARD=$(BOARD) defconfig' to apply it"; \
		echo "      (that discards anything menuconfig changed)"; \
	fi
	@for f in $(FRAGMENTS); do \
		if [ -f $(BUILD)/sdkconfig ] && \
		    grep '^CONFIG_' boards/fragments/$$f.config | grep -qvxFf $(BUILD)/sdkconfig; then \
			echo "note: $(BUILD)/sdkconfig was not made with the $$f fragment;"; \
			echo "      run 'make BOARD=$(BOARD) FRAGMENTS=\"$(FRAGMENTS)\" defconfig' to apply it"; \
		fi; \
	done

# Like the kernel's: throw the configuration away and take the board's.
defconfig: need-board
	@rm -f $(BUILD)/sdkconfig
	@$(IDF) reconfigure >/dev/null
	@echo "$(BUILD)/sdkconfig is now boards/$(BOARD).defconfig plus the defaults"

menuconfig: need-board
	@$(IDF) menuconfig

flash: need-board need-port
	@$(IDF) -p $(PORT) flash
	@$(EXPORT) && python3 tools/settime.py "$(PORT)" --boot

# the board's clock from the PC's
time: need-port
	@$(EXPORT) && python3 tools/settime.py "$(PORT)"

term monitor: need-port
	@$(IDF) -p $(PORT) monitor

size:
	@$(IDF) size

clean:
	@$(IDF) fullclean

distclean:
	rm -rf build sdkconfig sdkconfig.old managed_components
	$(MAKE) -C lang clean

boards:
	@echo "boards (BOARD=...):"; ls boards/*.defconfig | sed 's|boards/||;s|\.defconfig||;s|^|  |'
	@echo "fragments (FRAGMENTS=...):"; ls boards/fragments/*.config | sed 's|boards/fragments/||;s|\.config||;s|^|  |'

font:
	python3 tools/mkfont.py > drivers/tty/font5x8.h

need-board:
	@test -f boards/$(BOARD).defconfig || { echo "no such board: $(BOARD)"; $(MAKE) -s boards; exit 1; }
	@for f in $(FRAGMENTS); do \
		test -f boards/fragments/$$f.config || { echo "no such fragment: $$f"; $(MAKE) -s boards; exit 1; }; \
	done

need-port:
	@test -n "$(PORT)" || { echo "no board found: plug it in, or pass PORT=/dev/ttyACM0"; exit 1; }

hosttest:
	@$(MAKE) -s -C lang test
	@python3 tools/ai_ui_test.py
	@python3 tools/tty_keys_test.py
	@python3 tools/vt_render_test.py
	@python3 tools/ai_render_test.py
	@python3 tools/ai_stream_test.py
	@python3 tools/ai_dispatch_test.py
	@python3 tools/ai_file_read_test.py
	@python3 tools/ai_tools_test.py
	@python3 tools/ai_session_test.py
	@python3 tools/video_scanout_test.py
	@python3 tools/lcd_io_test.py
	@python3 tools/lcd_init_test.py
	@python3 tools/lcd_capture_test.py
	@python3 tools/audio_test.py
	@python3 tools/audio_init_test.py
	@python3 tools/flac_test.py
	@python3 tools/mp3_test.py
	@python3 tools/wav_test.py
	@python3 tools/pkg_test.py
	@python3 tools/auth_test.py
	@python3 tools/netconsole_test.py
	@python3 tools/serial_output_test.py
	@python3 tools/power_test.py
	@python3 tools/idle_time_test.py
	@python3 tools/idle_screen_test.py
	@python3 tools/proc_cleanup_test.py
	@python3 tools/lang_oom_test.py
	@python3 tools/nes_log_test.py
	@python3 tools/nes_palette_test.py
	@python3 tools/nes_lifecycle_test.py
	@python3 tools/jpeg_test.py
	@python3 tools/programs_test.py

# QEMU emulates neither the display, the card slot, USB nor the flash
# filesystems, so the qemu board leaves those out and puts / in RAM.
test: hosttest
	@$(MAKE) BOARD=qemu build
	@cd build/qemu && $(EXPORT) && \
		esptool --chip esp32s3 merge-bin --pad-to-size 16MB -o flash.bin @flash_args
	@python3 tools/shell_test.py build/qemu/flash.bin
	@python3 tools/script_test.py build/qemu/flash.bin
	@python3 lang/tests/device_test.py build/qemu/flash.bin

hwtest: need-port
	@$(EXPORT) && python3 tools/shell_test.py $(PORT)

progtest: need-port
	@$(EXPORT) && python3 tools/programs_test.py --board $(PORT)

scripttest: need-port
	@$(EXPORT) && python3 tools/script_test.py $(PORT)

langtest: need-port
	@$(EXPORT) && python3 lang/tests/device_test.py $(PORT)

push pull: need-port
	@$(EXPORT) && python3 tools/xfer.py $@ "$(PORT)" "$(FILE)" "$(DEST)"

# Convert a video into a clip the board plays (tools/mkvideo.py; its
# options in VIDEOARGS), kept in clips/, then send it to ~/video. The
# serial port takes a clip of a minute or two in about as long; an
# episode, a gigabyte and more, would take hours, so a big one is left
# for the card (SEND=1 sends it anyway).
CLIP = clips/$(notdir $(basename $(FILE))).ptv
video:
	@test -n "$(FILE)" || { echo "make video FILE=film.mkv [VIDEOARGS=...]"; exit 1; }
	@mkdir -p clips
	@python3 tools/mkvideo.py "$(FILE)" "$(CLIP)" $(VIDEOARGS)
	@size=$$(stat -c %s "$(CLIP)"); \
	if [ $$size -gt 104857600 ] && [ -z "$(SEND)" ]; then \
		echo "too big to send over USB ($$((size / 360000000 + 1)) h): copy it into video/ on the card"; \
	elif [ -z "$(PORT)" ]; then \
		echo "no board found: make push FILE=$(CLIP) DEST=~/video/$(notdir $(CLIP)) once it is plugged in"; \
	else \
		$(EXPORT) && python3 tools/xfer.py push "$(PORT)" "$(CLIP)" "~/video/$(notdir $(CLIP))"; \
	fi

SECONDS ?= 600
stress: need-port
	@$(EXPORT) && python3 tools/stress_test.py $(PORT) $(SECONDS)
