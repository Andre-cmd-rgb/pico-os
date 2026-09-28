# PocketType

A small Unix-like system for the ESP32-S3 (and, built but not yet run, the
ESP32-P4): a kernel with processes, pipes,
signals, `/dev` and `/proc`, a POSIX shell, over a hundred commands (grep, sed,
find, sort and the rest behave like GNU's) and a full-screen editor,
drawn in PIXELTAPE's amber CRT colors on an ILI9341 screen.

```
[0.00] PocketType 1.0-beta2 (esp-idf v6.1) #1 SMP Sep 25 2026 16:45:53
[0.05] mem: 218 KB internal, 6138 KB psram available
[0.05] power: the keyboard answered 2498 of 2498 polls while asleep
[0.05] power: woke on the keyboard, key 0x6f
[0.09] memtest: 6016 KB PSRAM ok (40 ms)
[0.09] serial: console on native USB
[0.09] proc: 16 process slots, programs on core 1, stacks in PSRAM
[0.09] cpufreq: ondemand, 80-240 MHz, idle sleep off
[0.43] lcd: ili9341 (alt init) on SPI2 at 80 MHz, 320x240
[0.43] vt: 53x23 console
[0.48] es8311: codec at 0x18, 16000 Hz
[0.49] audio: 16000 Hz mono, speaker and microphone
[0.49] button: GPIO0 switches terminals (hold for the first)
[0.49] cardkb: keyboard found at 0x5f
[0.50] rootfs: / is littlefs on flash:storage, 13160 KB free of 13248 KB
[0.56] battery: 3.86 V, 58%; a 2500 mAh cell, 90 mohm (a guess), 0.0 cycles
[0.56] tmpfs: /tmp holds up to 2048 KB, taken from RAM as it is used
[0.63] sd: AGGCE 60906 MB on 4-bit sdmmc, mounted on /mnt/sd and /home/andre
[0.63] wifi: radio off, no saved network; `wifi on` starts it
[0.63] init: 124 programs, starting shell
```
(a real boot of the Freenove board, woken from `suspend` by a key on the
CardKB, under a second to the shell; a board with parts missing says so for
each and comes up anyway)

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
| `waveshare-esp32-p4-wifi6` | **Waveshare ESP32-P4-WIFI6**: ESP32-P4 with 32 MB of PSRAM and 32 MB of flash, microSD, ES8311 audio, USB 2.0 OTG, Wi-Fi through an ESP32-C6. No screen or battery: the console is its USB-C serial port. Builds; not yet run on a board. |
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
make flash          # flash and reset, then set its clock from the PC's
make time           # set the board's clock from the PC's
make term           # serial console; Ctrl-] quits
make test           # PC tests, then QEMU with the device suites
make hosttest       # the PC tests: the pico language, JPEG, the text programs
make hwtest         # the shell suite on the board
make progtest       # grep, sort, find and the rest on the board, against GNU's
make scripttest     # shell control flow (if/for/while/case, functions)
make langtest       # the pico language on the device
make push FILE=...  # copy a file to the board's home (DEST=... elsewhere)
make pull FILE=...  # copy a file back (DEST=... to name it)
make stress         # hammer the board for 10 minutes (SECONDS=...)
```

Every target takes `BOARD=`, and each board builds into its own
`build/<board>/`, so switching between them costs nothing. The board file
picks the console too: the Freenove board has one USB-C port and talks over
USB Serial/JTAG, while the DevKitC flashes through its **COM** port and
keeps the native **USB** port for a keyboard (`CONSOLE=usb` moves the
console there instead).

If the board does not appear as `/dev/ttyACM*`, hold **BOOT**, tap
**RESET**, release **BOOT**, then `make flash`.

## Using it

There is a printed guide to all of this: **[docs/booklet/](docs/booklet/)**
has `guide.pdf` (using the machine) and `pico.pdf` (its language), each two
A4 sheets printed on one side and folded into pocket booklets.

The first start asks a few questions before the shell: your name (the
prompt's, and your home directory's, `/home/NAME`), the time zone, a
Wi-Fi network to join, the time if nothing has set it, and the colours.
Enter keeps the offered answer and Esc leaves the rest for later;
`setup` asks it all again, and `setup timezone`, say, one thing. The
answers are in `/etc/user` and `/etc/timezone` (the Wi-Fi's in
`/etc/wifi`, the colours in `/etc/theme`).

The shell is on the screen and on the serial console at the same time. Type
on the CardKB, a USB keyboard or the PC; all three work together.

There are **four terminals**, as on a Linux console: Ctrl-A then a digit
switches, or `chvt 2`. Each keeps its own screen and its own shell, and a
shell only starts when you first switch to its terminal. The status line
(at the top, or the bottom: `theme bar bottom`) says which one you are
on, the time, the next alarm, the battery and the network. Programs on
different terminals run at the same time: music keeps playing while a
game or a clip on another terminal makes its own sound, and the two are
mixed. A game or a clip draws only while its own terminal is showing (a
game pauses when you switch away), an alarm takes the screen from either
until it is answered, and one stopped with `kill -STOP` gives its
terminal back to the shell until `fg`.

**Themes**: `theme list` shows eleven. Amber and green CRTs, nord,
dracula and tokyonight are dark only, since dark suits this panel far
better; gruvbox, solarized, catppuccin, rosepine and mono have a dark
and a light half; paper is light only, for daylight. `theme gruvbox
light` switches, `theme auto 7:00 20:00` is light by day and dark by
night on a theme with both halves, `theme set bg
#101418` gives any colour (text, background, the status line, the
cursor, any of the sixteen) your own value, and `theme cursor underline`
or `theme bar bottom` change the rest. It is all kept in `/etc/theme`
and applied before the first line of the boot log.

| Key | In the shell |
|---|---|
| Ctrl-A then 1-4 | switch terminal (`chvt` does the same) |
| Ctrl-A then z | doze: screen and radio off, everything kept; a key brings it back where it was (Fn 0 on the CardKB) |
| Ctrl-A then Up | look back through what scrolled off (Up/Down, Fn Up/Down to the ends, Esc to return); Shift-PgUp on a USB keyboard |
| Fn 5 / Fn 6 | brightness down / up (CardKB) |
| Fn 7 / Fn 8, Fn 9 | volume down / up; mute (CardKB). Both are remembered in `/etc/power` |
| Tab | complete commands, their subcommands and options, and file names of the kind the command takes (quoted when they have spaces) |
| Up / Down | history (saved in `~/.sh_history`) |
| Esc | clear the line; stops a running command |
| Ctrl-C | stop a running command |
| Ctrl-L | clear the screen |

The CardKB has no Ctrl key, so **Esc** does the Ctrl-C job and opens the menu
in `edit`.

**Notes to study from**: put Markdown or text files in `~/notes` on the card
(a folder per subject works) and run `notes`. Headings, bold, lists, quotes,
code and tables are laid out for the 53-column screen; Space and `b` turn the
page, `o` lists the headings to jump to, `/` searches, `+` and `-` set the
backlight for reading in the dark, and each note reopens where you left it
(`~/.notes_pos`). `notes -p FILE` prints the laid-out text instead.

**The diary**: `alarm 7:00 mo-fr wake up` sets an alarm and `alarm 25m tea`
a timer. When one comes due it rings whatever is running: beeps that grow
louder, the backlight pulsing and the status bar showing what it is; any key
snoozes it for nine minutes and Esc stops it, and `suspend` wakes the board
for it. The status bar shows the next alarm within a day. `todo` keeps a
checklist in `~/todo.md` (`todo add studiare storia @fri`), open things
first and the soonest due at the top. `calendar` is a month to move about
in, with each day's events from `~/calendar.txt` and what is due from the
to-do list; `calendar add 30/9 10:00 verifica !15` adds an event with a
reminder chime 15 minutes before, and `every mo,we` and `yearly 2/10` repeat.
All three files are plain text, to edit on a PC as well. `cal` prints months
as util-linux's does.

### Commands

`help` lists them, `help <command>` explains one.

| | |
|---|---|
| files | `ls cat cp mv rm mkdir rmdir touch pwd stat find du df mount umount sync` |
| text | `head tail wc grep sed sort uniq cut tr tee diff cmp hexdump more edit notes` |
| diary | `alarm todo calendar cal` |
| checksums | `cksum md5sum sha1sum sha256sum sha512sum` |
| scripts | `echo printf seq expr xargs basename dirname realpath yes` |
| system | `ps top kill free dmesg uptime uname whoami hostname date time sleep env which clear reboot theme` |
| power | `cpufreq power suspend poweroff led battery` |
| shell | `cd exit export unset set local read eval source type command alias unalias trap jobs fg bg history sh` |
| sound | `play rec beep volume` |
| network | `wifi ntp ping wget curl passwd modem sms` |
| hardware | `bench lcdtest keytest backlight rotate i2cdetect mkfs screenshot chvt` |
| pictures | `view video` |
| games | `nes pad` |

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
| `/home/NAME` | your home directory: the SD card again, under the name `setup` asked for |
| `/tmp` | 2 MB RAM disk: fast, and empty after every boot |
| `/mnt/sd` | the microSD card (FAT32, readable on a PC); `umount /mnt/sd` before removing it |
| `/dev` | `null zero urandom stdin stdout stderr tty`, plus `audio` where there is a codec: raw 16-bit mono, write to play it, read to record |
| `/proc` | `cpuinfo kmsg meminfo mounts uptime version`, plus `net` and `modem` where those drivers are on |

At boot the kernel runs `/etc/rc` if it exists, then a login shell prints
`/etc/motd` and runs `/etc/profile` and `~/.profile`. Put your own commands in
`/bin` or `/mnt/sd/bin` (both on `PATH`) as scripts.

### Programs (the pico language)

pico is a small C-like language that compiles on the laptop and on the board.
You write `.pico` files; `picoc file.pico` produces a bytecode executable that
the kernel runs directly, and `pico file.pico` compiles and runs in one step. It is
memory-safe (reference counting, bounds-checked arrays and strings) and a
mistake stops with the source line, not a crash. See
**[docs/LANGUAGE.md](docs/LANGUAGE.md)**, and **[docs/PICO-TUTORIAL.md](docs/PICO-TUTORIAL.md)**
to learn it from the start.

```sh
make push FILE=hello.pico    # send a source file to the board
picoc hello.pico && ./hello  # compile it there and run it
```

### Moving files on and off

`make push` copies a file to the board and `make pull` copies one back, over
the serial port, beside the console rather than through it: nothing is typed
on the board and the shell stays free while it goes, so you can carry on
using it. The transfer is base64 plus a CRC-32 a chunk, so any binary is
safe and corruption is caught and sent again (about 100 KB/s to the card;
for big clips, copying on the card itself is quicker). `push` puts the file
in the board's home directory unless you pass `DEST=` (`~/` means the
board's home there too, and so does a path not starting with /), and writes
to `DEST.part` first: the old file is replaced only once the whole new one
has arrived intact. Remember `/tmp` is a RAM disk, emptied on reboot.

```sh
make push FILE=hello.pico                # -> ~/hello.pico
make push FILE=notes.txt DEST=/mnt/sd/notes.txt
make pull FILE=~/hello.pico DEST=copy.pico
```

### Clips

The board plays its own clip format, `.ptv`: a JPEG a frame and plain PCM
sound, which is what a 240 MHz chip can decode at 30 frames a second. The
PC converts a video with ffmpeg, and fetches one first with yt-dlp; both
need to be on the PATH. A copy stays in `clips/`.

```sh
make video FILE=film.mp4                 # convert and send to ~/video
make yt URL=https://...                  # fetch, convert and send
python3 tools/mkvideo.py film.mp4 film.ptv --fps 24   # just convert
python3 tools/mkvideo.py ep.mkv ep.ptv --fill --audio eng  # full screen, the English track
```

A 16:9 picture is letterboxed to 320x176 unless `--fill` covers the
screen and cuts the sides; `--audio` picks one sound track of several
(a language, or a number from 0). A film graded with grey blacks looks
faded on the panel, with black specks in its shadows: `--black 16` makes
lumas up to 16 black and stretches the rest back out. `--loud -14`
brings quiet film sound up and evens it out for the small speaker. A long clip is big (a 45-minute
episode is about a gigabyte): copying it onto the card on the PC is
quicker than `make push`, which goes at about 100 KB/s.

## Speed and power

`cpufreq` picks a policy the way Linux governors do:

| Policy | CPU (ESP32-S3) |
|---|---|
| `performance` | always 240 MHz |
| `ondemand` (default) | 240 MHz whenever a task runs, 80 MHz when both cores are idle |
| `powersave` | always 80 MHz, about 3x slower |

240 MHz is the ESP32-S3's maximum; it cannot be overclocked. The ESP32-P4
runs at 400, 200 and 100 MHz (360, 180 and 90 before revision 3).

**When nobody is using it** the backlight -- the largest draw on the
board -- is the first thing to go: with no key for 3 minutes the screen
dims to a quarter, after 4 it goes dark with the panel asleep (the key
that lights it again does nothing else, since nobody could see what it
would do). Dark, the chip light-sleeps between the keyboard's polls, as
a phone does, with everything kept; what must not sleep keeps it awake
for as long as it runs -- sound playing, a PC or a keyboard on USB. After 15 minutes with nothing running -- every terminal at its
prompt, no sound playing, no PC on USB -- it suspends. A program left
open, a note being read or a file being edited, keeps it awake with the
screen dark instead, since waking from deep sleep is a fresh boot. A clip
playing in front holds the screen lit. **Ctrl-A z dozes**: the screen goes
dark at once, the radio rests and the chip light-sleeps between the
keyboard's polls, with everything kept in memory, so a key brings it back
exactly where it was (deep sleep cannot: it powers the memory off, and the
chips' own state cannot be put back, so waking from it is a fresh boot). `power dim 3m blank 4m suspend
15m` sets the times (`never` turns one off), kept in `/etc/power`. While
the screen is dark the kernel polls its keyboard and its waits ten
times less often, so the chip can sleep between them. Away from its
saved network, Wi-Fi tries again after 5 s, then 10, 20... up to every
10 minutes, with the radio stopped and its memory given back in between.

- **`power sleep on`**: light sleep whenever nothing is happening. RAM is kept
  and the system carries on at the next key, timer or transfer. It never sleeps
  while a PC is connected to the native USB console or a USB keyboard is
  plugged in, and on the COM-port console the first key after a quiet spell can
  be lost. `power` prints how much time was spent in each mode.
- **`suspend`**: deep sleep until any key on the CardKB, the side button,
  the next alarm, or `-t SECONDS`. Waking is a fresh boot (under a second to
  the shell), so save first. The low-power RISC-V core asks the CardKB for a
  key every 100 ms, doing the I2C by hand on whichever RTC pins it is wired
  to (16/15 on the Freenove board), so a board in a case needs no button.
  `poweroff` is the same sleep with no timer and no alarms, woken only by
  **Enter** (or the buttons), so a key knocked in a bag does not turn it on.

**The battery.** There is no current sensor, so `battery` works from the
voltage and what the board is doing: an estimate of the current (backlight,
CPU, radio, light sleep, or the charger's), the cell's resistance learned
from the board's own charger coming and going (300 mA, known), the
backlight's current learned from its steps once that is known, and the
charge counted between readings and pulled towards what the sag-corrected
voltage says in a lithium cell's table. The level is shown in tenths and
moves the way the current goes, never against it: it does not fall on the
charger or rise on the cell. A reboot keeps it where it was. `battery`
shows the level and the cell's **cycles** (what was used since it was
fitted, over its capacity); `battery -v` shows everything, including its
**health** (what a discharge from full shows it holds, against the first
ones when it was new: it needs a full charge and a run down to 20% before
it has a figure). After fitting a new cell, `battery -c MAH` with its
capacity starts all of that again. A cell run flat powers off, and wakes
every quarter of an hour to see whether a charger has come.

The readings can be a few percent off -- the ADC and the divider allow for
it, and near full 3% of the voltage is a third of the charge -- so measure
the cell with a meter once and tell it: `battery -m 4.075`. On the Freenove
board the charger (a TP4054, no status pin) does not start on a cell above
about 4.05 V, so plugged in at 85% it says "on USB, not charging" rather
than pretending. `battery -l` is the power log: every start (and why),
every sleep and each half hour awake, with the level, kept in
`/etc/power.log` -- where a night's charge went can be read back from it.
The RGB LED is the charge light the board lacks: lit while charging, green
when full, dark otherwise.

**The clock** sets itself from the network whenever Wi-Fi connects, and
again every hour, and `make flash` or `make time` on the PC sets it too.
The chip keeps time through restarts, and through deep sleep on an RC
oscillator that wanders with temperature; the system learns how far from
what the network says after each sleep, and allows for it on the next wake.
For a power cut the time is saved in `/etc/clock` every hour and before
`poweroff` and `suspend`, so the next boot starts from there rather than
from 1970.

`bench` on this board, at 240 MHz:

| Test | Result |
|---|---|
| CPU, CRC-32 | 18.9 MB/s |
| FPU, Mandelbrot 320x240 | 272 ms |
| memcpy, internal RAM | 352 MB/s |
| memcpy, PSRAM | 30 MB/s |
| start and reap a process | 0.19 ms |
| `/` write / read, 512 KB file | 64 KB/s / 6.7 MB/s |
| `/` create / delete a small file | 104 ms / 11 ms |
| `/` write, a line at a time | 1,290 lines/s |
| `/tmp` write, a line at a time | 49,300 lines/s |
| display bus, full frames | 53 fps (7.8 MB/s) on the Freenove panel at 80 MHz |

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

- **The battery's current is estimated, never measured.** The level,
  the time left, cycles and health all rest on it. What the charger puts
  into a 2500 mAh cell is not yet measured (the board file says 50 mA,
  from a 200 mAh cell); the voltage keeps the level honest either way.
- **A terminal cannot say when a key is released**, so in games a press
  counts as held for 150 ms. Fine for menus, poor for platformers. A
  Bluetooth pad is the fix, and the driver is written (`pad scan`, `pad
  connect`, the buttons mapped in `/etc/gamepad`), but it has not met a real
  pad yet. Everything else on the Freenove board has been run on the real
  thing: display, speaker, microphone, SD card, battery sensing, Wi-Fi
  (`make hwtest`, 86 checks).
- **The CardKB is only as good as its wiring.** It shares SDA 16 / SCL 15
  with the codec on the chip's weak internal pull-ups; a long or loose
  Grove cable drops reads. The driver rides out short runs of them.
- **The parallel-bus panel on the Uno shield never worked** — the end of
  docs/WIRING.md says what was tried.
- **The modem is half tried.** A SIM800L (the ESP-800L board) answers:
  it is found at any speed, its news is heard (restarts, the SIM, the
  network), sleep mode and `modem off` work, and it has heard networks
  at -81 dBm. On this board's wiring it loses its SIM once it starts to
  transmit, which is its supply (docs/WIRING.md); SMS and PPP data have
  not been sent over the air yet.
- **MP3 decoding is not ours.** FLAC and WAV are (`codec/`); MP3 leans on
  minimp3 in `third_party/`, which says why.

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
lang/       the pico language: compiler, VM, tests and examples
tools/      on the PC: file transfer, video conversion, tests, the font
docs/       wiring, architecture, the language
```

## Licence

Copyright 2026 Andre. PocketType is free software under the GNU General
Public License, version 2 (`LICENSE`): use it, change it and share it, as
long as what you share goes out under the same licence, with its source.
The NES emulator core in `third_party/nofrendo/` is GPL v2 as well; the MP3
decoder in `third_party/minimp3/` is public domain (CC0).
