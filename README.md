# PocketType

A small Unix-like system for the ESP32-S3: a kernel with processes, pipes,
signals, `/dev` and `/proc`, a POSIX shell, over a hundred commands (grep, sed,
find, sort and the rest behave like GNU's) and a full-screen editor,
drawn in PIXELTAPE's amber CRT colors on an ILI9341 screen.

```
[0.00] PocketType 0.1.0 (esp-idf v6.1) #1 SMP Sep 23 2026 13:58:56
[0.05] mem: 183 KB internal, 8173 KB psram available
[0.10] memtest: 8064 KB PSRAM ok (51 ms)
[0.10] serial: console on native USB
[0.10] proc: 16 process slots, programs on core 1
[0.10] cpufreq: ondemand, 80-240 MHz, idle sleep off
[0.10] battery: 3.56 V (8%) on GPIO9
[0.45] lcd: ili9341 (alt init) on SPI2 at 80 MHz, 320x240
[0.45] vt: 53x23 console
[0.51] es8311: codec at 0x18, 16000 Hz
[0.51] audio: 16000 Hz mono, speaker and microphone
[0.51] button: GPIO0 switches terminals (hold for the first)
[0.51] cardkb: keyboard found at 0x5f
[0.54] rootfs: / is littlefs on flash:storage, 13168 KB free of 13248 KB
[0.56] tmpfs: /tmp holds up to 2048 KB, taken from RAM as it is used
[0.63] sd: AGGCE 60906 MB on 4-bit sdmmc, mounted on /mnt/sd and /home/andre
[0.63] wifi: radio off, no saved network; `wifi on` starts it
[0.63] init: 124 programs, starting shell
```
(a real boot of the Freenove board, under two thirds of a second to the
shell; a board with parts missing says so for each and comes up anyway)

## Hardware

Drivers are separate options, as in Linux: a board file switches on the ones
that board has, and `make menuconfig` can change any of them.

```sh
make boards                         # list them
make BOARD=freenove-fnk0104b flash  # the default
```

| Board | What it is |
|---|---|
| `freenove-fnk0104b` | **Freenove ESP32-S3 Display**: 2.8" 240x320 IPS over SPI, microSD (SDMMC), ES8311 codec with speaker and microphone, RGB LED, battery charger. 16 MB flash, 8 MB PSRAM. |
| `devkit-uno-shield` | An S3 DevKitC with a 2.4" ILI9341 Uno shield on an 8-bit parallel bus. The panel never displayed anything; kept for the parallel-bus code. |
| `qemu` | No hardware at all: the test suites. |

Optional on either board: an M5Stack CardKB over I2C, and a USB keyboard
where there is a spare USB port.

Wiring, power and first-boot checks: **[docs/WIRING.md](docs/WIRING.md)**.

## Build and flash

ESP-IDF v6.1 lives in `~/esp/esp-idf`. The Makefile wraps `idf.py`:

```sh
make boards         # the board files and what they switch on
make menuconfig     # System, and one option per driver
make                # build
make flash          # flash and reset
make term           # serial console; Ctrl-] quits
make test           # PC tests, then QEMU with the device suites
make hosttest       # the PC tests: the "a" language, JPEG, the text programs
make hwtest         # the shell suite on the board
make progtest       # grep, sort, find and the rest on the board, against GNU's
make scripttest     # shell control flow (if/for/while/case, functions)
make langtest       # the "a" language on the device
make push FILE=...  # copy a file to the board's home (DEST=... elsewhere)
make pull FILE=...  # copy a file back (DEST=... to name it)
make stress         # hammer the board for 10 minutes (SECONDS=...)
```

Every target takes `BOARD=`, and each board builds into its own
`build-<board>/`, so switching between them costs nothing. The board file
picks the console too: the Freenove board has one USB-C port and talks over
USB Serial/JTAG, while the DevKitC flashes through its **COM** port and
keeps the native **USB** port for a keyboard (`CONSOLE=usb` moves the
console there instead).

If the board does not appear as `/dev/ttyACM*`, hold **BOOT**, tap
**RESET**, release **BOOT**, then `make flash`.

## Using it

The shell is on the screen and on the serial console at the same time. Type
on the CardKB, a USB keyboard or the PC; all three work together.

There are **four terminals**, as on a Linux console: Ctrl-A then a digit
switches, or `chvt 2`. Each keeps its own screen and its own shell, and a
shell only starts when you first switch to its terminal. The bottom line
of the screen says which one you are on, the time, the battery and the
network.

| Key | In the shell |
|---|---|
| Ctrl-A then 1-4 | switch terminal (`chvt` does the same) |
| Tab | complete commands and file names |
| Up / Down | history (saved in `~/.sh_history`) |
| Esc | clear the line; stops a running command |
| Ctrl-C | stop a running command |
| Ctrl-L | clear the screen |

The CardKB has no Ctrl key, so **Esc** does the Ctrl-C job and opens the menu
in `edit`.

### Commands

`help` lists them, `help <command>` explains one.

| | |
|---|---|
| files | `ls cat cp mv rm mkdir rmdir touch pwd stat find du df mount umount sync` |
| text | `head tail wc grep sed sort uniq cut tr tee diff cmp hexdump more edit` |
| checksums | `cksum md5sum sha1sum sha256sum sha512sum` |
| scripts | `echo printf seq expr xargs basename dirname realpath yes` |
| system | `ps top kill free dmesg uptime uname whoami hostname date time sleep env which clear reboot` |
| power | `cpufreq power suspend poweroff led battery` |
| shell | `cd exit export unset set local read eval source type command alias unalias trap jobs fg bg history sh` |
| sound | `play rec beep volume` |
| network | `wifi ntp ping modem sms` |
| hardware | `bench lcdtest keytest backlight i2cdetect mkfs screenshot chvt` |
| games | `nes` |

The shell is a POSIX sh: pipes (`|`), redirection (`<`, `>`, `>>`, `2>`,
`2>&1`) and here-documents (`<<EOF`), lists (`;`, `&&`, `||`), background
jobs (`&`), variables and parameter expansion (`$HOME`, `$?`, `${NAME:-x}`,
`${NAME%.txt}`), `$(...)` and `$((...))`, quoting, `~` and `*`/`?`/`[...]`
globbing, `if`/`while`/`until`/`for`/`case`, functions with `local`, and
`set -e`, `-u`, `-x` and `-o pipefail`, `trap` (EXIT, INT, TERM) and
aliases. Job control is there too: Ctrl-Z stops the program in front,
`jobs` lists what is stopped or in the background, `fg` and `bg` bring
one back, and `%1` names one for `kill` and `wait`. Scripts run with
`sh file` or directly, if they start with `#!` or end in `.sh`.

### Filesystem

| Path | What it is |
|---|---|
| `/` | 13 MB LittleFS on the internal flash; safe if the power dies mid-write |
| `/home/andre` | your home directory |
| `/tmp` | 2 MB RAM disk: fast, and empty after every boot |
| `/mnt/sd` | the microSD card (FAT32, readable on a PC); `umount /mnt/sd` before removing it |
| `/dev` | `null zero urandom stdin stdout stderr tty`, plus `audio` where there is a codec: raw 16-bit mono, write to play it, read to record |
| `/proc` | `cpuinfo kmsg meminfo mounts uptime version`, plus `net` and `modem` where those drivers are on |

At boot the kernel runs `/etc/rc` if it exists, then a login shell prints
`/etc/motd` and runs `/etc/profile` and `~/.profile`. Put your own commands in
`/bin` or `/mnt/sd/bin` (both on `PATH`) as scripts.

### Programs (the `a` language)

`a` is a small C-like language that compiles on the laptop and on the board.
You write `.al` files; `ac file.al` produces a bytecode executable that the
kernel runs directly, and `a file.al` compiles and runs in one step. It is
memory-safe (reference counting, bounds-checked arrays and strings) and a
mistake stops with the source line, not a crash. See
**[docs/LANGUAGE.md](docs/LANGUAGE.md)**.

```sh
make push FILE=hello.al      # send a source file to the board
ac hello.al && ./hello       # compile it there and run it
```

### Moving files on and off

`make push` copies a file to the board and `make pull` copies one back, over
the serial console. The transfer is base64 plus a CRC-32, so any binary is
safe and corruption is caught. `push` puts the file in the board's home
directory unless you pass `DEST=` (`~/` means the board's home there too), and
writes to `DEST.part` first: the old file is replaced only once the whole
new one has arrived intact. Remember `/tmp` is a RAM disk, emptied on reboot.

```sh
make push FILE=hello.al                  # -> ~/hello.al
make push FILE=notes.txt DEST=/mnt/sd/notes.txt
make pull FILE=~/hello.al DEST=copy.al
```

## Speed and power

`cpufreq` picks a policy the way Linux governors do:

| Policy | CPU |
|---|---|
| `performance` | always 240 MHz |
| `ondemand` (default) | 240 MHz whenever a task runs, 80 MHz when both cores are idle |
| `powersave` | always 80 MHz, about 3x slower |

240 MHz is the ESP32-S3's maximum; it cannot be overclocked.

Two more ways to save battery. Neither has been measured with a meter yet.

- **`power sleep on`**: light sleep whenever nothing is happening. RAM is kept
  and the system carries on at the next key, timer or transfer. It never sleeps
  while a PC is connected to the native USB console or a USB keyboard is
  plugged in, and on the COM-port console the first key after a quiet spell can
  be lost. `power` prints how much time was spent in each mode.
- **`suspend`**: deep sleep. The low-power RISC-V core reads the CardKB every
  100 ms and wakes the system on a key; `suspend -t 60` also wakes after a
  minute. Waking is a fresh boot (under a second to the shell), so save first.

`bench` on this board, at 240 MHz:

| Test | Result |
|---|---|
| CPU, CRC-32 | 18.9 MB/s |
| FPU, Mandelbrot 320x240 | 272 ms |
| memcpy, internal RAM | 358 MB/s |
| memcpy, PSRAM | 30 MB/s |
| start and reap a process | 0.11 ms |
| `/` write / read, 512 KB file | 125 KB/s / 4.1 MB/s |
| `/` create / delete a small file | 100 ms / 18 ms |
| `/` write, a line at a time | 2,600 lines/s |
| `/tmp` write, a line at a time | 49,300 lines/s |
| display bus, full frames | 55.7 fps (8.2 MB/s) on the Freenove panel at 80 MHz |

The `/` numbers are what the flash sustains once its blocks have been used:
every 4 KB must be erased before it is written again. Freshly formatted flash
writes about four times faster (500 KB/s, 9,300 lines/s) until it has been
filled once. Keep big or busy files on the SD card or in `/tmp`.

The root filesystem was chosen by measurement: FAT on the same flash wrote at
91 KB/s (fresh) and took 215 ms to create and 148 ms to delete a 1 KB file.

Open files are buffered in RAM (4 KB each, allocated only when a file is
actually used), which is what makes reading and writing a line at a time
reasonable. The buffer is written out when the file is closed; `sync` also
makes the filesystem commit its own caches to the flash or card, and
`reboot`, `poweroff` and `suspend` sync first. Pulling the battery loses
whatever has not been written out. `umount /mnt/sd` refuses while a file is
open on the card.

## Not done yet

Be aware of these before relying on it:

- **Waking from `suspend` with a key** is still untested.
- **A terminal cannot say when a key is released**, so in games a press
  counts as held for 150 ms. Fine for menus, poor for platformers; a
  Bluetooth pad is the fix and is not written yet. Everything else on the
  Freenove board has now been run on the real thing: display, speaker,
  microphone, SD card, battery sensing, Wi-Fi (`make hwtest`, 86 checks).
- **The CardKB is only as good as its wiring.** It shares SDA 16 / SCL 15
  with the codec on the chip's weak internal pull-ups; a long or loose
  Grove cable drops reads. The driver rides out short runs of them.
- **The parallel-bus panel on the Uno shield never worked** — the end of
  docs/WIRING.md says what was tried.
- **The modem has never seen a module.** The AT layer, SMS and the PPP
  data link are written and compile, and the no-module path is tested
  (it says so and gets out of the way), but nothing has been plugged in.
- **MP3 decoding is not ours.** FLAC and WAV are (`codec/`); MP3 leans on
  minimp3 in `third_party/`, which says why.
- **The clock starts at 1970 on every boot.** Set it with `date -s`.
- **`kill -9` of a process stuck in a tight loop** deletes its task after half a
  second and can leak whatever it held.

## Source tree

```
main/       boot sequence and the menuconfig menu (Kconfig.projbuild)
kernel/     processes, files, pipes, signals, /proc, kernel log, system calls
boards/     one defconfig per board: which drivers it has, and on which pins
drivers/    ILI9341 (SPI or parallel), terminal, keyboards, storage, sound,
            battery, LED, Wi-Fi, modem, power and suspend
codec/      sound file decoders: WAV, FLAC (ours), MP3
emu/        the port layer for emulators: screen, sound, joypad
third_party/ the two things here somebody else wrote: an MP3 decoder and
            a NES emulator core
shell/      sh: parser, executor, line editor
bin/        the commands
lang/       the "a" language: compiler, VM, tests and examples
tools/      font generator, test harnesses, file transfer
docs/       wiring, architecture, the language
```
