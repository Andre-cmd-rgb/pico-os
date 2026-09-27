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
#   make video FILE=clip.mp4        convert a video and send it
#   make yt URL=https://...         fetch one from the web, convert and send it
#   make stress        hammer the board at PORT for SECONDS (default 600)
#
# BOARD picks which drivers and pins to start from (boards/*.defconfig, and
# boards/README.md); each board builds in its own directory, build/BOARD:
#
#   make BOARD=devkit-uno-shield flash term
#
# PORT defaults to the first /dev/ttyUSB* or /dev/ttyACM* found.

BOARD    ?= freenove-fnk0104b
BUILD    ?= build/$(BOARD)
IDF_PATH ?= $(HOME)/esp/esp-idf
PORT     ?= $(firstword $(wildcard /dev/ttyUSB*) $(wildcard /dev/ttyACM*))
EXPORT   := . $(IDF_PATH)/export.sh >/dev/null
IDF      := $(EXPORT) && idf.py -B $(BUILD) -D SDKCONFIG=$(BUILD)/sdkconfig \
	    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/$(BOARD).defconfig"

.PHONY: all build defconfig menuconfig flash time term monitor clean distclean size font boards \
	test hosttest hwtest progtest scripttest langtest push pull video yt stress need-port need-board

all: build

# The build date `uname` and the boot log show is compiled into ESP-IDF's
# app description, which is otherwise only rebuilt when IDF changes: so
# every build compiles it again, and the date says which firmware this is.
APP_DESC := $(BUILD)/esp-idf/esp_app_format/CMakeFiles/__idf_esp_app_format.dir/esp_app_desc.c.obj

build: need-board stale-config
	@rm -f $(APP_DESC)
	@$(IDF) build

# The board file only seeds a configuration that does not exist yet, so
# changing it later is silent unless someone says so.
stale-config:
	@if [ -f $(BUILD)/sdkconfig ] && \
	    [ boards/$(BOARD).defconfig -nt $(BUILD)/sdkconfig ]; then \
		echo "note: boards/$(BOARD).defconfig is newer than $(BUILD)/sdkconfig;"; \
		echo "      run 'make BOARD=$(BOARD) defconfig' to apply it"; \
		echo "      (that discards anything menuconfig changed)"; \
	fi

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

font:
	python3 tools/mkfont.py > drivers/tty/font5x8.h

need-board:
	@test -f boards/$(BOARD).defconfig || { echo "no such board: $(BOARD)"; $(MAKE) -s boards; exit 1; }

need-port:
	@test -n "$(PORT)" || { echo "no board found: plug it in, or pass PORT=/dev/ttyACM0"; exit 1; }

hosttest:
	@$(MAKE) -s -C lang test
	@python3 tools/jpeg_test.py | tail -2
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

# Convert a video into something the board can play, then send it over.
# The clip stays in clips/, so a board that is not plugged in loses nothing.
CLIP = clips/$(notdir $(basename $(FILE))).ptv
video: need-port
	@mkdir -p clips
	@python3 tools/mkvideo.py "$(FILE)" "$(CLIP)" $(VIDEOARGS)
	@$(EXPORT) && python3 tools/xfer.py push "$(PORT)" "$(CLIP)" \
		"~/video/$(notdir $(CLIP))"

# The same, starting from a web link (yt-dlp).
yt:
	@$(EXPORT) && python3 tools/ytgrab.py "$(URL)" --push $(if $(PORT),--port "$(PORT)") $(YTARGS)

SECONDS ?= 600
stress: need-port
	@$(EXPORT) && python3 tools/stress_test.py $(PORT) $(SECONDS)
