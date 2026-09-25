# Hacking on PocketType

## How it boots

`main/init.c` brings things up in dependency order:

1. kernel log, wake reason after a `suspend`, PSRAM self test
2. status LED, serial console, process table, CPU frequency policy, battery
3. display, terminal and console
4. sound, then the keyboards: CardKB, USB
5. `/` (the root filesystem), its standard directories, `/tmp`, `/mnt/sd`
6. `/etc/rc`, if it exists
7. a login shell, restarted whenever it exits

A driver that is switched off compiles to stubs returning `-ENODEV`, so the
boot sequence never changes shape from one board to the next.

Kernel messages go to the screen during boot. Afterwards they go to `dmesg`
and the serial port only, so they never scribble over the shell.

## Layers

```
bin/ shell/ lang/  programs            use pt/sys.h only (plus a few kernel.h calls)
kernel/            system calls, processes, files, pipes, /proc, klog
drivers/           lcd -> vt -> tty <- uart, cardkb, usbkbd;  flash, sd, audio
ESP-IDF            FreeRTOS, VFS/FAT/LittleFS, esp_lcd, USB host
```

Each top-level directory is an ESP-IDF component (`EXTRA_COMPONENT_DIRS` in
the root `CMakeLists.txt`).

## Boards and drivers

Every driver has its own Kconfig option under **Device drivers**, and every
board is a defconfig in `boards/` that switches on the ones it has and sets
their pins — the same split as Linux's drivers and `arch/*/configs`.

```sh
make boards                          # list
make BOARD=freenove-fnk0104b flash   # the default board
make BOARD=devkit-uno-shield build   # another, in its own build-<board>/
```

`SDKCONFIG_DEFAULTS` is `sdkconfig.defaults` plus the board file, so
`menuconfig` edits stay in that board's `sdkconfig` and never leak into
another board. The board file names the chip (`CONFIG_IDF_TARGET`), and
ESP-IDF adds that chip's own defaults after the common ones:
`sdkconfig.defaults.esp32s3` (octal PSRAM, the S3's caches, the ULP core,
XIP from PSRAM) or `sdkconfig.defaults.esp32p4`. Each chip keeps its own
`dependencies.lock.<chip>`, since the P4 pulls in ESP-Hosted for its Wi-Fi
(through the ESP32-C6 on its board) and the S3 does not.

Porting to another chip: a `sdkconfig.defaults.<chip>` and a board file,
then the places that know the chip -- `cpufreq.c` (its speeds),
`procfs.c` (`/proc/cpuinfo`), `uname`, and the pin ranges in
`main/Kconfig.projbuild`. On the P4 the key-wake in `suspend` is left out:
it is written for the S3's ULP RISC-V core, and the P4 has an LP core
with a different API.

Adding a driver: a Kconfig option that other options hang off, the source
under `drivers/<kind>/`, `-ENODEV` stubs in an `#else` so the rest of the
tree needs no `#if`, an entry in `drivers/CMakeLists.txt`, its prototypes in
`drivers/include/drivers/drivers.h`, and a call in `main/init.c`. A driver
with a character device calls `dev_register()` and appears in `/dev`.
Drivers sharing one I2C bus ask `i2c_bus_get()` for it by pin pair; the
first one to ask brings it up.

## Filesystems

`/dev` and `/proc` are served by the kernel (`struct pt_kernfs`). Everything
else goes through the mount table (`kernel/mount.c`): each mount has the
path users see and the ESP-IDF VFS prefix its files really live under, and a
path belongs to the mount with the longest matching path, as on Linux.

| Seen as | Lives under | Driver |
|---|---|---|
| `/` | `/rootfs` | `drivers/storage/rootfs.c`: LittleFS (default), FAT, or a RAM disk for QEMU |
| `/tmp` | `/tmpfs` | `drivers/storage/ramdisk.c`: FAT on PSRAM |
| `/mnt/sd` | `/sdcard` | `drivers/storage/sdcard.c`: FAT32 over SPI |

Mount points are real directories on `/`, created at boot before anything is
mounted on them. When the SD card is out, `/mnt/sd` is just that empty
directory. Only kernel code sees VFS prefixes; programs never do.

Files on these filesystems are buffered in `kernel/file.c` (4 KB of internal
RAM each, allocated on first use). `pt_close` writes the buffer out and
returns the error if that fails, so check it after writing anything that
matters. `vfs_sync_all` (`sync`) writes every buffer and `fsync`s every open
file. Each open file remembers its mount, and `sd_unmount` refuses while any
are open: after an unmount their descriptors would be reused by other files.

LittleFS is case-sensitive and replaces an existing file on rename, in one
power-safe step. FAT ignores case and refuses to rename over a file. Code
that renames (the editor, `mv`, `rx`) renames first and removes the old file
only when the rename fails with `EEXIST`.

## Terminals

There are several, and one screen. `drivers/tty/vt.c` keeps a `struct
screen` per terminal -- its text, its cursor, its dirty rows -- and paints
only the one in front; `drivers/tty/tty.c` keeps a `struct tty` per
terminal -- its input buffer, the line being edited, its foreground
process group -- and gives the keyboard to the one in front. init starts
a shell on a terminal the first time somebody switches to it.

Two rules came out of getting this wrong:

- **whoever sees the key does not paint.** Switching happens on a driver
  task with a small stack, so `vt_switch()` marks the screen and wakes
  the renderer, which is the only task that ever draws the terminal.
- **a terminal exists before it is in front.** Its buffers are made on
  first use, and the keyboard would otherwise write into nothing.

## Sound files

`codec/` decodes them, with one interface for every format the way a
filesystem driver plugs into the VFS: `codec_open()` offers the first
bytes of the file to each decoder until one claims them, and the caller
reads 16-bit frames. WAV and FLAC are written here; FLAC in particular is
a full decoder (bit reader with both check sums, fixed and LPC
predictors, Rice residuals, the three stereo decorrelations, any bit
depth) and its output is checked against ffmpeg bit for bit. MP3 is
minimp3 in `third_party/`, wrapped by `codec/mp3.c`; it keeps a 17 KB
scratch buffer on the stack, which is why `play` asks for a 32 KB one.

`play -n` decodes without playing and prints the rate, the speed and a
CRC-32 of the samples: that is how a decoder is checked on a board with
no speaker.

## Games

`emu/` is the port layer and `third_party/nofrendo/` is the emulator
itself. The core asks its host for three things and this supplies them:
a function to hand a finished frame to (`blit`), a buffer to draw into,
and the joypad state each frame. Two traps, both paid for once:

- the video buffer has to be set **after** the cartridge is loaded,
  because inserting one resets the machine and the reset clears the
  pointer -- set it first and the core silently skips every frame;
- `nes_emulate()` already mixes the frame's sound into the APU's own
  buffer, so the host reads that rather than calling `apu_process()`
  again, which would generate a second frame from a state that has
  moved on.

The 256x240 picture sits in the middle of the 320x240 panel. Drawing
every frame needs 7.4 MB/s and the bus does 8.2 at 80 MHz, so the
default draws every other frame: the emulation stays at 60 Hz and the
screen gets 30. `nes -f 0` draws them all and runs at about 38.

## Screenshots

Nothing keeps a copy of the screen -- the panel is the only copy, which
is what makes the terminal cheap. So `lcd_capture_begin()` arms a buffer
that `lcd_draw()` also writes into, the screen is made to draw itself
again, and `lcd_capture_save()` writes a BMP. Armed, it costs one memcpy
per draw; idle, one pointer test.

## Network

`drivers/net/wifi.c` is the radio plus a supplicant task that walks
`/etc/wifi` until something answers, and `/proc/net`. `wifi off` deinits
the stack rather than merely stopping it, because those buffers are 30 KB
of internal RAM.

`drivers/net/modem.c` is a serial modem: an AT command reader and writer,
SMS in text mode, and a PPP link over lwip's pppos for mobile data. While
PPP has the port, AT commands return -EBUSY. The module is probed on a
task at boot, because a SIM7600 needs ten seconds before it answers
anything.

## Sound

`drivers/audio/es8311.c` configures the codec over I2C: the register
sequence follows Espressif's own driver, and because the I2S peripheral
always feeds MCLK at 256x the sample rate, the vendor's clock table
collapses into the seven writes in `clock_config()`.

`drivers/audio/audio.c` owns the I2S link and `/dev/audio`. The wire frame
is stereo because the codec wants two slots, so mono samples are doubled on
the way out and every second sample is taken on the way in. The amplifier
is only enabled while something plays, and `audio_stop()` waits for the DMA
buffers to drain first, or the last word is cut off.

Both directions share one I2S controller, and the receiver is a slave to
the transmitter's clock -- which is what the "rx switched from master to
slave for full-duplex" line at start-up means. So recording enables the
transmitter too, silent and with the amplifier off, or the microphone gets
no clock and every read times out. That one only showed up on real
hardware: with no codec fitted, `rec` fails earlier for a different
reason.

## Power

- `drivers/power/battery.c`: the cell voltage through the board's divider,
  curve-fitted where the chip is calibrated. Its task warns on the screen,
  and at the critical level syncs, unmounts and deep-sleeps. Under
  `BATTERY_NO_CELL_MV` there is no cell and the board is on USB. The
  percentage comes from an estimate of the current (`draw_ma`: the board's
  measured idle and backlight draw from menuconfig, the chip's datasheet
  figures for the CPU, the radio's measured share, or the charger's set
  current), the cell's resistance learned from steps in that current, and
  the charge counted between readings and pulled towards the
  sag-corrected voltage.
- `drivers/power/cpufreq.c`: the frequency policy and idle light sleep through
  `esp_pm`. With `CONFIG_PM_PROFILING`, `power` prints every lock and the time
  spent in each mode; a lock stuck at 100% is what keeps the CPU at 240 MHz.
  Drivers must hold locks only while working (the LED enables its RMT channel
  just for each update for this reason).
- `drivers/power/suspend.c` and `drivers/power/ulp/wake.c`: deep sleep, woken
  by the side button (ext1, with the RTC domain's pull-up), or -- where the
  CardKB is wired to GPIO 0-3 -- by a program on the low-power RISC-V core
  that reads it over the RTC I2C controller every 100 ms.
- `kernel/clock.c`: the time saved to `/etc/clock` hourly and before sleep,
  put back at boot after a power cut; `wifi.c` starts SNTP on every new
  address.
- `drivers/misc/alarm.c`: alarms, timers and the calendar's reminders, in
  `/etc/alarms`. Its task rings the one due whatever is running: it claims
  the speaker (`audio_claim`: other writers' sound goes nowhere, at its
  own pace), pulses the backlight and takes over the status bar;
  `tty_input()` and the side button hand it keys first while it rings.
  `power_suspend()` sets the timer to wake a moment before the next one,
  and a boot woken by the timer looks back ten minutes for it. A snooze is
  a dated one-off alarm, so it survives a suspend. The `alarm` command
  (bin/alarm.c) only parses what is typed; `bin/dates.c` holds the day and
  time parsing that alarm, todo and calendar share.

## Processes

A process that exits leaves its stack for the idle task to free, so a
program that spawns in a tight loop can run the heap down before that
happens. `proc_spawn()` therefore waits a couple of milliseconds and
tries again rather than failing something that is only briefly out of
memory.


A process is a FreeRTOS task on **core 1** with Unix bookkeeping: pid, parent,
process group, 16-slot fd table, working directory, environment, argv, and a
list of its allocations. Drivers and the renderer run on core 0, so a
runaway program can't freeze the screen or keyboards.

- `pt_spawn` resolves a name in this order: built-in program, a path containing
  `/`, then `$PATH`. Files are handed to the first loader that claims them.
- Children get fds 0–2 only. Nothing else is inherited, so pipes close properly.
- `pt_malloc` memory is freed when the process exits.
- **Signals** are delivered when the process makes its next system call.
  SIGINT and SIGTERM end it with status 128+signal unless it called
  `pt_sigcatch(true)`. SIGKILL gets 500 ms; after that the reaper task deletes
  it, unless it holds a FreeRTOS mutex (a filesystem's or a driver's): then it
  is let run until it has put the lock down, since a lock taken to the grave
  hangs everyone after it. Code a program runs -- a library such as the NES
  core included -- should allocate with `pt_malloc`, so a kill frees it all.
- Ctrl-C (or Esc on the CardKB) signals the terminal's foreground process
  group, which the shell sets to each pipeline it runs.

## System calls

`kernel/include/pt/sys.h` is the entire interface a program gets, ABI
version 1. Calls return negative errno values (`-ENOENT`), never set `errno`,
and take paths relative to the working directory. The same set is exported
as a function table, `pt_sys`, for loaded programs.

## Adding a command

Put it in `bin/`, or in a new file listed in `bin/CMakeLists.txt`:

```c
#include "util.h"

PT_PROGRAM(hello, "print a greeting\nusage: hello [name]")
{
	pt_printf("hello, %s\n", argc > 1 ? argv[1] : "world");
	return 0;
}
```

That's all. It registers itself before `app_main`, and `help`, tab completion
and the shell find it. Use `PT_PROGRAM_STACK(name, kb, help)` if it needs more
than 8 KB of stack. Full-screen programs use `pt/keys.h`: `pt_tty_raw`,
`pt_readkey`, `pt_tty_size`.

## The terminal

`drivers/tty/vt.c` understands:

- `\r \n \b \t`
- `ESC 7` / `ESC 8`, and `ESC c` (reset)
- CSI `A B C D G d H f J K L M s u m`
- `?25h` / `?25l` (cursor visibility)

SGR supports bold, dim, inverse, and colors 30–37, 90–97, 40–47 and 100–107.
All 16 colors map to shades of the theme; see `theme[]`. Text is UTF-8. The
font covers ASCII plus à è é ì ò ù ç £ € ° § and █; anything else shows as `?`.

Regenerate the font after editing `tools/mkfont.py` with `make font`.

Raw mode (`PT_TTY_SETRAW`) passes keys through as the bytes a VT terminal
sends. `PT_TTY_SETTIMEOUT` is how `pt_readkey` tells Esc from an arrow key.

## Running executables

`kernel/include/pt/program.h` defines `struct pt_loader`: a `probe` that
claims a file by its first bytes, and an `exec` that runs inside the new
process. Two are registered:

- **scripts** (`shell/sh.c`): files starting with `#!` or ending in `.sh`.
- **`a` programs** (`lang/port_pt.c`): files starting with `\x7fAL`, the
  bytecode `ac` writes. The loader checks the file's CRC-32 and verifies
  every function before the VM runs it, and the VM makes the same system
  calls a C program does, so signals, Ctrl-C and `ps` work unchanged.

The language itself is described in [LANGUAGE.md](LANGUAGE.md). Its core
(`lang/`) builds into the firmware and, with `make -C lang`, into `ac` and `a`
for the PC; `lang/port.h` is the only platform interface. Another format,
WebAssembly for instance, would be one more loader registered the same way.

## Testing

```sh
make hosttest                      # the a compiler and VM, JPEG, the text programs
make -C lang asan                  # the a tests under AddressSanitizer and UBSan
make test                          # hosttest, then QEMU: no board needed
make hwtest PORT=/dev/ttyACM0      # the shell suite on a board
make progtest PORT=/dev/ttyACM0    # the text programs on a board, against GNU's
make scripttest PORT=/dev/ttyACM0  # if/for/while/case, functions, scripts
make langtest PORT=/dev/ttyACM0    # the a language on the board
make stress PORT=/dev/ttyACM0      # 10 minutes of process, file and signal churn
```

The board suites work in `/tmp/work` or `$HOME` and clean up. The harness
opens the native USB port without toggling RTS/DTR, so the board keeps
running between runs.

### QEMU

This builds a QEMU variant (`tools/sdkconfig.qemu`) and runs
`tools/shell_test.py`, which types commands into the shell and checks the
output. The variant leaves out what QEMU can't emulate: display, SD, USB,
octal PSRAM, the flash filesystems or the low-power core. `/` becomes a RAM
disk instead.

It needs Espressif's QEMU (about 30 MB to download) and libslirp:

```sh
python ~/esp/esp-idf/tools/idf_tools.py install qemu-xtensa
sudo pacman -S libslirp
```

`tools/shell_test.py` finds QEMU under `~/.espressif/tools/qemu-xtensa`, or
takes its path from `$QEMU`.

Add a check to `run_tests()` in `tools/shell_test.py` whenever you add a command.

### The text programs on the PC

`grep`, `sort`, `find`, `cut`, `tr`, `printf`, `expr` and the other programs
that need nothing but system calls also build for the PC: `tools/host_pt.c`
supplies the calls over POSIX, and `tools/programs_test.py` compiles them
with it under the sanitizers, runs each case in `CASES` through bash with
ours first on `PATH` and then with GNU's, and compares. A case that means
to differ from GNU gives its own expected output. Then a few hundred random
regular expressions go through our `grep` and GNU's.

`make progtest` runs the same cases on a board, through its own shell. A
new text program goes into `SOURCES` and `PROGRAMS` there, with cases.
