# Hacking on PocketType

## How it boots

`main/init.c` brings things up in dependency order:

1. kernel log, why it started (a wake from sleep, a crash, power-on...) --
   and back to sleep at once if a flat cell woke only to find it still flat
   -- then the PSRAM self test
2. status LED, serial console, process table, CPU frequency policy
3. display, terminal and console
4. sound, then the keyboards: CardKB, USB
5. `/` (the root filesystem); the user's name and time zone
   (`kernel/user.c`), then the standard directories, `/home/NAME` among
   them; the clock and the battery, which read what they saved there;
   `/tmp`, `/mnt/sd`
6. `setup -f`, the first start's questions, while there is no `/etc/user`
7. `/etc/rc`, if it exists
8. a login shell, restarted whenever it exits

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
make BOARD=devkit-uno-shield build   # another, in its own build/<board>/
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
with a different API. The S3's RTC slow clock is the 17.5 MHz RC divided
by 256 (`sdkconfig.defaults.esp32s3`), which keeps time through deep sleep
better than the 136 kHz one; changing it needs `make defconfig`, or the
same lines in `build/<board>/sdkconfig`.

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

A program that draws on the panel itself -- `nes`, `video`, `view`,
`lcdtest`, `bench`'s screen test -- **holds its own terminal's screen**
with `vt_hold_screen(true)`: the renderer leaves that terminal alone, and
the program draws only between `vt_screen_begin()` and `_end()`, and
only when begin says its terminal is in front. When the terminal comes
back, `vt_screen_gen()` has changed and the program paints everything
again. Each terminal has its own holder: with one for all of them, a
game on the first terminal and a clip started on the second overwrote
each other's claim, and the game drew over the clip. A program's helper
task is no program and has no terminal of its own, so it draws with the
`_on()` calls and the terminal the program got from `vt_screen_mine()`
(the clip's blitter does). `vt_redraw()` never paints over a terminal a
program holds; it asks that program to paint again instead.

What else two programs on two terminals share is made safe the same
way: the speaker is a stream each (below); the screen kept lit is a
count (`power_keep_screen`); the CPU's speed is a count too
(`cpufreq_boost`: a clip lifts the policy's floor to its top while it
plays, and never saves and restores the policy, which `cpufreq` on
another terminal may have changed meanwhile).

## Sound files

`codec/` decodes them, with one interface for every format the way a
filesystem driver plugs into the VFS: `codec_open()` offers the first
bytes of the file to each decoder until one claims them, and the caller
reads 16-bit frames. WAV and FLAC are written here; FLAC in particular is
a full decoder (bit reader with both check sums, fixed and LPC
predictors, Rice residuals, the three stereo decorrelations, any bit
depth) and its output is checked against ffmpeg bit for bit. MP3 is
minimp3 in `third_party/`, wrapped by `codec/mp3.c`; it keeps a 17 KB
scratch buffer on the stack, which is why `play` asks for a 24 KB one
(it was seen to use 18). `play` is the one program allowed on either
core (`PT_PROGRAM_ANYCORE`): decoding is a sixth of a core, and on the
programs' core it slowed a game on another terminal from 60 frames a
second to 57.

## The speaker is shared

`drivers/audio/audio.c` is a mixer. Every task that writes gets a stream,
a ring of its samples in PSRAM at its own rate; a kernel task on core 0
takes a block from each, brings it to the rate on the wire (the highest
any stream with samples wants) by linear interpolation, adds them up and
hands them to I2S, whose DMA paces everything. A writer blocks while its
ring is full, so a program timed by its sound still runs at its rate.
`audio_set_latency()` sizes the ring: a second for `play` (a card busy
writing a screenshot never runs it dry), 50 ms for the NES and a clip,
whose sound has to keep up with the picture. `audio_stop()` plays the
caller's queue out, `audio_discard()` drops it (a pause), and a stream
whose process has gone is freed by the mixer. The alarm still takes the
speaker over (`audio_claim`), and the others go on at their pace,
unheard. After 5 s of silence the codec goes into standby, and before
deep sleep `audio_sleep()` puts it there and holds the amplifier's
shutdown pin.

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
screen gets 30. `nes -f 0` draws them all and runs at about 38. Its row
buffer is in PSRAM, aligned for the DMA: in internal RAM it took 12 KB
that music on another terminal left no room for.

## Clips without tearing

The ILI9341 refreshes from its memory a row at a time along its own
portrait rows, whatever orientation is sent; a landscape frame takes
16 ms to send and a refresh 12, so every refresh during a send showed
parts of two frames along slanting seams. The TE pin is not wired on the
Freenove board, but the panel's SDO is (GPIO13), so `io_spi.c` adds a
second, 4 MHz SPI device for reads -- chip select taken by hand for the
moment -- and `lcd_draw_native()` asks the panel which line it is
refreshing (0x45, Get Scanline; a dummy bit first, so the count arrives
shifted by one). The clip player turns each frame into the panel's own
order (`canvas_blit_native`, 16-pixel tiles) and sends it in bands of
rows, each only once the refresh has passed it, so no refresh meets a
half-sent band. Two things only the panel could show: its row order bit
(MY) turns the refresh round too, so it is never changed between frames;
and with MY set the refresh runs from the last row to the first.
`lcdtest tear up|down|land` flashes red and blue frames sent each way:
all at once is right, a seam is tearing.

The panel also sat on the wrong SPI controller until 1.0: the board file
says `SPI_HOST=2` for SPI2, and ESP-IDF numbers from SPI1, so 2 was SPI3,
which has no pins of its own and went through the GPIO matrix at twice
its rated 40 MHz. `io_spi.c` maps the name to the number now.

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

`bin/ai.c` is a client for OpenRouter's chat API: a POST through
esp_http_client with the answer streamed back as server-sent events,
laid out as it comes (words wrapped to the screen, Markdown turned into
SGR, links into numbered sources). Each mode sends a list of models and
OpenRouter moves down it when one is busy. In code mode the request
carries five tools; the program loops, running what the model asks for
(each change and command confirmed by the user, every path held inside
the project by `inside()`) and sending the results back, until it
answers in words. The conversation is kept as JSON text and trimmed from
the front a question at a time. `bin/json.c` is the JSON it needs: a
value found by its path over the text, with no tree and no allocation,
and a builder that escapes strings.

`bin/pkg.c` installs programs from the pico-os-packages repository: its
`index.txt` (name, version, size, SHA-256, about) and each package's
source, fetched with `wget` (or copied, when `~/.config/pkg/repo` names a
folder), checked with PSA's SHA-256, and compiled by `picoc` into `~/bin`.
The index, what is installed and the sources are in `~/.config/pkg`.

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

`rec` records at 16 kHz, and the recording takes the codec's clock at
its own rate: whatever plays meanwhile is resampled to it by the mixer.
(It used to take the wire's rate, so a lesson recorded after a song was
44.1 kHz.) What it records goes through `codec/voice.c` on the way to the
card: a 90 Hz high-pass, then a 256-point FFT every 8 ms whose bands are
scaled down by how little they stand above the noise (Martin's minimum
statistics for the noise, Ephraim and Malah's decision-directed gain,
never below -15 dB), then a leveller that follows the speech's level in
decibels, and a limiter at -1 dB. The codec's own gain riding is held off
meanwhile (`audio_mic_alc_hold`, counted): two levellers in a row fight,
and its ramp up in the first seconds was taken for speech. About 9% of a
core. `rec -n` records as before.

## Power

- `drivers/power/battery.c`: the cell voltage through the board's divider,
  the middle half of 64 samples every 5 s. Under `BATTERY_NO_CELL_MV` or
  over 4.3 V there is no cell (the board is on USB). The level comes from
  an estimate of the current (`draw_ma`: the board's measured idle and
  backlight draw from menuconfig, the chip's datasheet figures for the CPU,
  the radio's share, or the charger's set current), the cell's resistance
  learned from steps in that current, and the charge counted between
  readings and pulled towards the sag-corrected voltage in the table of a
  lithium cell at rest (`curve[]`, 5% steps). The charger is known for
  certain while a PC talks over USB; otherwise from the step the voltage
  takes, looked for closely just after the PC goes quiet (a PC that
  suspends the port has not been unplugged), and failing that from the
  trend. The level is kept in permille, follows the current at once and
  walks against it (`STILL`, `CREEP`). While a charge goes in it stops at
  99%; the charger going away while it held the cell full (a TP4056 turns
  its light green and stops at a tenth of its current, a small step that
  is watched for, or the cell falling from where it was held) is 100%
  (`charge_finished`), and for two hours after, the cell's voltage settling
  back is left to only nudge the level. A charger at work also keeps the
  board from suspending, so that it sees the end. A restart carries everything over in
  RTC memory (`kept`); a power-on or a wake from sleep starts from the
  voltage. Cycles are the charge counted out over the capacity; health
  is what a discharge from full to 20% says it holds, over what the first
  two said. `/etc/battery` keeps the capacity, resistance, and that life.
  Flat and falling: `power_off_empty()`.
- `drivers/power/cpufreq.c`: the frequency policy and idle light sleep through
  `esp_pm`. With `CONFIG_PM_PROFILING`, `power` prints every lock and the time
  spent in each mode; a lock stuck at 100% is what keeps the CPU at 240 MHz.
  Drivers must hold locks only while working (the LED enables its RMT channel
  just for each update for this reason).
- `drivers/power/suspend.c` and `drivers/power/ulp/wake.c`: deep sleep
  (`power_suspend`, `power_off`, `power_off_empty`), woken by the side button
  (ext1, with the RTC domain's pull-up), the timer, or a CardKB key: the
  low-power RISC-V core asks the CardKB for a key every 100 ms, bit-banging
  I2C on the RTC pins (any of GPIO 0-21), and counts the polls it asked and
  had answered, which the next boot logs. `suspend` wakes on any key,
  `poweroff` on Enter (`ulp_want`). A flat cell's sleep wakes every 15
  minutes on the timer and goes straight back unless the cell has risen
  past 3.65 V, before the screen lights. `power_boot_reason()` logs why
  the chip started. `power_quiesce()` turns idle light sleep off first:
  while it is on, esp_pm keeps the timer armed as a wake source for its
  naps, and deep sleep inherited it -- a suspend with the screen dark woke
  "on the timer" the moment it slept.
- `kernel/user.c`: the user's name, and so `/home/NAME`, and the time
  zone, from `/etc/user` and `/etc/timezone` (`bin/setup.c` asks for
  them). The SD card's second mount has no path of its own
  (`mount_path()`): it is wherever the home is, and a new name renames
  the directory on the flash and moves the mount at once. The mount table
  reads the path on every lookup, from any task, so a change is made in a
  second copy and switched in with one store. Programs ask `user_home()`
  (`home_dir()` in `bin/`), never a name built in. What the system keeps
  for the user goes in folders of the home, not its top: `pt_home_file()`
  gives `~/.config/sh_history`, `~/.config/notes_pos`,
  `~/agenda/calendar.txt` and `~/agenda/todo.md`, and moves a file an
  older system left at the top (`~/.sh_history`) into its place.
- `kernel/clock.c`: the time saved to `/etc/clock` hourly and before sleep,
  put back at boot after a power cut; `wifi.c` starts SNTP on every new
  address and hourly after. Going to sleep marks the moment in RTC memory
  (`clock_sleeping`); waking corrects the sleep by the learned drift of the
  RTC clock, and the next network time after a sleep of 30 minutes or more
  that began with the clock just set teaches the drift (`drift` in
  `/etc/clock`). `clock_sleep_us()` stretches a timer wake-up by it.
  `tools/settime.py` (`make time`, and `make flash` after flashing) sets the
  clock from the PC with `date -s @SECONDS`.
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

A process's stack is in **PSRAM** (`xTaskCreatePinnedToCoreWithCaps`),
not internal RAM: there are megabytes of the one and about 200 KB of the
other, and with stacks in internal RAM three programs and a video were
enough to leave no room to start `free`. Two things a PSRAM stack cannot
do, because they turn the cache off: touch the flash, and go to sleep or
restart. `kernel/internal.c` does them on `kflash`, a kernel task with a
small internal stack on the programs' core. Every `esp_flash_*` write
and erase is wrapped at link time (`--wrap`, `kernel/CMakeLists.txt`), so
LittleFS, NVS and anything else end up there without knowing; kernel code
that has to sleep or restart from a process calls
`on_internal_stack(fn, arg)` or `restart_now()`. A caller whose stack is
internal already (a kernel task) goes straight through. Flash reads need
none of this while the program runs from PSRAM (XIP). `bench` measured
the same with stacks in either place.

A task cannot free the stack it is standing on, so a process that exits
suspends itself and the reaper deletes it (`vTaskDeleteWithCaps`). A
program that spawns in a tight loop can run ahead of that, so
`proc_spawn()` waits a couple of milliseconds and tries again rather than
failing something that is only briefly out of memory.


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
font covers ASCII, the letters of the Western European languages, Greek,
the common signs of maths and money, arrows, and the status line's icons;
look-alikes that text is full of (a non-breaking hyphen, thin spaces, a
minus sign) are drawn as the plain character, and anything else shows as
`?`. A glyph is five columns; an icon two cells wide (the Wi-Fi sign) also
sets bit 5 of its rows, the gap column, which `draw_cell()` then fills.

Regenerate the font after editing `tools/mkfont.py` with `make font`.

Raw mode (`PT_TTY_SETRAW`) passes keys through as the bytes a VT terminal
sends. `PT_TTY_SETTIMEOUT` is how `pt_readkey` tells Esc from an arrow key.

## Running executables

`kernel/include/pt/program.h` defines `struct pt_loader`: a `probe` that
claims a file by its first bytes, and an `exec` that runs inside the new
process. Two are registered:

- **scripts** (`shell/sh.c`): files starting with `#!` or ending in `.sh`.
- **pico programs** (`lang/port_pt.c`): files starting with `\x7fAL` (from
  when the language was called a), the bytecode `picoc` writes. The loader checks the file's CRC-32 and verifies
  every function before the VM runs it, and the VM makes the same system
  calls a C program does, so signals, Ctrl-C and `ps` work unchanged.

The language itself is described in [LANGUAGE.md](LANGUAGE.md). Its core
(`lang/`) builds into the firmware and, with `make -C lang`, into `picoc` and
`pico` for the PC; `lang/port.h` is the only platform interface. Another format,
WebAssembly for instance, would be one more loader registered the same way.

## Testing

```sh
make hosttest                      # the pico compiler and VM, JPEG, the text programs
make -C lang asan                  # the a tests under AddressSanitizer and UBSan
make test                          # hosttest, then QEMU: no board needed
make hwtest PORT=/dev/ttyACM0      # the shell suite on a board
make progtest PORT=/dev/ttyACM0    # the text programs on a board, against GNU's
make scripttest PORT=/dev/ttyACM0  # if/for/while/case, functions, scripts
make langtest PORT=/dev/ttyACM0    # the pico language on the board
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
