# pico-os on the perf branch: what got faster, lighter, longer-lasting

This file compares `perf`'s HEAD with the commit it started from, f8c62a6
(master when the work began). Every number below was measured on a PC
with no board attached, by one of the methods listed under [How it was
measured](#how-it-was-measured). Anything that needs the board is in [To
check on the board](#to-check-on-the-board), and changes a user would
notice are in [Proposals](#proposals). Nothing here changes what the
system does. Each commit says what it changed and by how much.

## How it was measured

- **Link map (LM):** `idf.py size` of each board's build. The
  component registry cannot be reached from the machine this was done on,
  and USB host, MSC and LittleFS come from it. So base and HEAD were both
  built with the same fragment: root on FAT, no USB keyboard or MSC
  (`IDF_COMPONENT_MANAGER=0`). The absolute numbers are a little under a
  real `make size`, but every difference is the same, since this branch
  touches none of those components.
- **perfbench (PB):** `tools/perfbench.py`. The real decoders and the
  real mixer, built for the S3 and run in QEMU with `-icount`, so the same
  code gives the same count on every run. These are QEMU's cycles, not
  the board's: no caches, no PSRAM waits. Each row carries a CRC-32 of
  its output, and every CRC is the same before and after.
- **QEMU shell (QS):** the `qemu` board's image (no display, audio or
  Wi-Fi; root on a RAM disk), run under QEMU with `-icount shift=0` so
  that its clock follows the instructions run. It is driven through
  `tools/board.py`: `free` at the prompt, then `time` on shell commands,
  three runs each, all three the same unless a range is shown.
- **Host (H):** `make hosttest` and `make -C lang bench` on the PC.
  The pico VM's instruction counts come from valgrind, in its commits.
- **Harness (HN):** throwaway C programs, not kept: the mixer's handshake
  on two threads, and the environment against a model of the old code.
- **Estimate (E):** worked out from object sizes or from ESP-IDF's
  sources. Each one is listed again under "To check on the board".

## 1. Internal RAM

| | base | HEAD | how |
|---|---:|---:|---|
| static internal RAM (IRAM + .data + .bss), Freenove | 146,914 B | 125,166 B | LM |
| the same, devkit-uno-shield | 121,174 B | 107,674 B | LM |
| the same, qemu | 81,890 B | 70,506 B | LM |
| QEMU shell: internal heap (total) | 399 KB | 410 KB | QS |
| QEMU shell: internal free at the prompt | 316 KB | 332 KB | QS |
| QEMU shell: low mark at the prompt | 240 KB | 260 KB | QS |
| QEMU shell: low mark after the file tests (sort, cp) | 236 KB | 259 KB | QS |

The QEMU board has no Wi-Fi, display or sound, so it cannot show what
those drivers take at run time. On the Freenove board, internal heap
also comes back from these:

| what no longer takes internal RAM at run time | bytes | how |
|---|---:|---|
| FreeRTOS timer task: 2 KB stack, TCB, queue (never used) | ~2,600 | E |
| default event loop (Wi-Fi's events): stack, TCB, queue | ~3,000 | E |
| NVS page cache and key hashes | ~1,000 | E |
| audio mixer: 3 KB stack and two 160-frame buffers | 4,992 | LM/E |
| terminal renderer's stack | 4,096 | E |
| 16 process slots × 2 semaphores | ~2,800 | E |
| each terminal opened: key buffer and two mutexes | ~1,200 | E |
| each file open on / or /tmp: its 4 KB buffer | 4,096 | E |
| each process: its arguments and environment | 150+ | E |

Taken together, with Wi-Fi, the screen, sound and one shell up, that is
about **21.7 KB of static RAM plus about 19 KB of heap**: roughly 40 KB
more internal RAM free from boot, and more again while files are open.
The board's `free` will confirm it.

## 2. PSRAM

| | base | HEAD | how |
|---|---:|---:|---|
| program copied to PSRAM at boot (64 KB pages: .text + .rodata), Freenove | 23 + 8 pages | 23 + 8 pages | LM |
| the same, before SoftAP was dropped | | 24 + 8 pages | LM |
| static PSRAM (.ext_ram.bss), Freenove | 29,444 B | 41,432 B | LM |
| QEMU shell: PSRAM used at the prompt | 1,373 KB | 1,383 KB | QS |
| `play`/`music`: the mono copy of every pass | 2 KB | none (only `play -n`) | code |
| pico: a system buffer for every line read or formatted | 1 per line | none (pooled strings) | H |

The kernel now uses more PSRAM than before, by exactly what moved there
from internal RAM: about 12 KB of statics (7 KB of it is the NES
emulator) and about 19 KB of run-time objects, out of 8 MB. The program
had grown by a page (the faster FLAC, JPEG and mixer code took `.text`
from 22.9 to 23.05 pages); dropping the Wi-Fi driver's unused SoftAP
half brought it back to 22.3 pages.
Real reductions in PSRAM would change something a user notices, so they
are listed under Proposals: the mixer's one-second rings are the large
item.

## 3. Static RAM and image size

Bytes, from the link map. "Internal" is DIRAM: IRAM code, .data, .bss
and the 1 KB of vectors.

| board | | IRAM | .data | .bss | internal | PSRAM .bss | flash code | .rodata | image |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| freenove-fnk0104b | base | 101,147 | 26,883 | 17,856 | 146,914 | 29,444 | 1,501,388 | 489,984 | 2,120,860 |
| | HEAD | 95,083 | 22,959 | 6,096 | 125,166 | 41,432 | 1,460,400 | 490,784 | 2,070,684 |
| | change | −6,064 | −3,924 | −11,760 | −21,748 | +11,988 | −40,988 | +800 | −50,176 |
| devkit-uno-shield | base | 84,675 | 25,983 | 9,488 | 121,174 | 28,004 | 1,303,568 | 384,748 | 1,800,432 |
| | HEAD | 78,699 | 22,347 | 5,600 | 107,674 | 32,096 | 1,257,400 | 384,820 | 1,744,724 |
| | change | −5,976 | −3,636 | −3,888 | −13,500 | +4,092 | −46,168 | +72 | −55,708 |
| qemu | base | 56,559 | 17,103 | 7,200 | 81,890 | 15,556 | 804,060 | 310,684 | 1,189,864 |
| | HEAD | 51,723 | 13,627 | 4,128 | 70,506 | 19,156 | 808,056 | 313,052 | 1,187,916 |
| | change | −4,836 | −3,476 | −3,072 | −11,384 | +3,600 | +3,996 | +2,368 | −1,948 |
| waveshare-esp32-p4-wifi6 | base | 86,882 | 18,034 | 16,256 | 121,172 | 13,344 | 1,184,844 | 379,732 | 1,673,982 |
| | HEAD | 86,766 | 14,890 | 12,960 | 114,616 | 17,200 | 1,192,482 | 382,860 | 1,681,488 |
| | change | −116 | −3,144 | −3,296 | −6,556 | +3,856 | +7,638 | +3,128 | +7,506 |

On the Freenove board, the internal RAM saved comes from:

- **IRAM −6,064 B:** ring-buffer ISR code (2.7 KB), the octal-flash
  driver (2.2 KB) and the I2C ISR (0.8 KB) moved to flash.
- **.data −3,924 B:** programs and completions are const now (3.3 KB);
  the octal driver (0.5 KB).
- **.bss −11,760 B:** driver statics that only tasks touch (3.5 KB), the
  NES emulator (7 KB) and bench's CRC table (1 KB) moved to PSRAM.

The image is **50,176 B** smaller on the Freenove board:

- SoftAP off saves 52.7 KB, the octal driver 2.5 KB and the timer
  task 1.6 KB.
- The rest of the branch adds about 7 KB, most of it code for speed.
  5.7 KB of that is the FLAC predictor, unrolled for each order.

The P4 gets none of the S3's configuration changes, and here it has no
Wi-Fi either (`esp_wifi_remote` comes from the registry). So it shows
only what the code itself saves in internal RAM, 6.6 KB, and what the
faster code costs in flash, 7.5 KB.

The Freenove board with the BLE fragment builds without warnings too.
At HEAD it uses 130,542 B of internal RAM and the image is 2,431,616 B;
it was not built at base.

## 4. Speed

| | base | HEAD | how |
|---|---:|---:|---|
| boot: PSRAM tested once, not twice | | ~250 ms sooner (est.) | E |
| wake from suspend/poweroff: image not re-hashed | | ~70 ms sooner (est.) | E |
| boot/wake: program copied to PSRAM | | 52 KB less to copy | LM |
| QEMU: boot to "starting shell" (after IDF's PSRAM test, so it cannot show it) | 0.70 s | 0.70 s | QS |
| QEMU: `sh -c` 2000-turn counter loop | 68 ms | 62-63 ms | QS |
| QEMU: 312 assignments in a `for` | 5 ms | 5 ms | QS |
| QEMU: start `true` 100 times (program start) | 51 ms | 50-51 ms | QS |
| QEMU: write 512 KB to /tmp through a pipe | 56-58 ms | 55-56 ms¹ | QS |
| QEMU: read 512 KB from /tmp / from the RAM root | 9-10 / 9 ms | 9-10 / 9 ms | QS |
| QEMU: `sort` 512 KB | 80-82 ms | 80-81 ms | QS |
| shell: prompt after a command | after the history write | before it | code |
| console: status line sent | every second | when it changes (~1/min) | code |
| pico: `s = s + x` to 100 KB | 160 ms | 1 ms | H |
| pico: `out = out + a + ": " + str(i) + "\n"`, 20,000 rounds | 1,254 ms | 3 ms | H |
| pico: read 1 MB by lines ×5 | 67.3 M instr. | 45.2 M instr. | H (valgrind) |
| pico: `find`/`contains` near the end of 99 KB, ×1000 | 81 / 292 ms | 24 / 23 ms | H |
| pico: `make -C lang bench` (CPU, FPU, arrays: base and HEAD alternated, 3 runs each) | | the same within the PC's noise; `sin+cos+sqrt` 25-36 → 21-29 ms | H |

The counter loop is faster because an assignment to the last variable
in the environment, the usual case in a loop, now writes over its old
value. Before, every assignment rebuilt the whole environment.

¹ One of the three runs printed 6 ms; the script took the wrong line.
Ten more runs on each image all wrote the same 512 KB (`wc -c`,
`cksum`).

File I/O speed did not change; the same commands take the same time.
Two changes affect file I/O, both on the board only:

- An open file on LittleFS or /tmp has its buffer in PSRAM now. This
  saves internal RAM. Those filesystems copy through buffers of their
  own anyway.
- The shell's history is written once the next prompt is up.

Where the board could read files faster is under Proposals.

## 5. Sound

QEMU cycles (PB), the same audio decoded, every CRC unchanged:

| | base | HEAD | change |
|---|---:|---:|---:|
| FLAC 44.1 kHz/16, 6 s | 3.30 M | 1.74 M | −47.3% |
| FLAC 96 kHz/24, 3 s | 5.45 M | 2.85 M | −47.7% |
| MP3 44.1 kHz 192k stereo, 6 s | 5.36 M | 4.57 M | −14.7% |
| MP3 44.1 kHz 96k mono, 6 s | 3.70 M | 3.30 M | −10.7% |
| WAV 44.1/16 and 48/24 | 0.38 / 0.26 M | 0.38 / 0.26 M | 0 |
| `play`'s 2:1 filter, 2 s of 192 kHz | 3.00 M | 2.49 M | −16.9% |
| mixer + speaker stage, 4 s of 44.1 kHz | 0.96 M | 0.48 M | −50.2% |
| mixer + headphone stage, 4 s of 44.1 kHz | 0.97 M | 0.44 M | −54.6% |

- **The writer side:** writes go into the ring without a division per
  frame.
- **Writer wake-ups:** a writer with a full ring wakes once its request
  fits, not after every block. Measured with the real code on two
  threads (HN), music's writes went from 3,000 wakes in 3,000 blocks to
  469, with no timeouts or underruns. A game's went from 3,000 to 652.
- **After a song:** the output no longer restarts for 145 ms of silence
  at the end of every song, clip and game sound.
- **Memory:** the mixer's 3 KB stack and 1.9 KB of buffers moved from
  internal RAM to PSRAM. `play` no longer keeps a 2 KB mono buffer
  outside `play -n`. `play -n`'s checksum takes 17 instructions a byte
  instead of 46.

## 6. Video

| | base | HEAD | change | how |
|---|---:|---:|---:|---|
| a clip's 320×120 slice (24 slices, two kinds of picture) | 4.78 M | 3.61 M | −24.6% | PB |
| progressive cover at 1/8 | 0.761 M | 0.768 M | +0.9% | PB |
| JPEG decode on this PC (x86, 320×240), 3 runs each | 0.38-0.45 ms | 0.37-0.42 ms | the same within noise | H |

The progressive cover is 0.9% slower: the decoder's register use came out
differently after the clamp change. It is decoded once per cover, not per
frame. On the PC (x86) the decoder takes the same time as before: the
changes are for the S3's instructions and registers.

- **The decoder:** each block clears only the coefficients it wrote.
  Clamps are the core's MIN/MAX instructions. The bit buffer stays in
  registers. Colour conversion has a function of its own. Lines never
  shown are not transformed at all.
- **A paused or hidden clip** gives back its 240 MHz boost, so the
  governor can drop to 80 MHz.
- **Panel sending:** the status line is no longer resent every second.
  That is 6.4 KB a second off the SPI bus whenever the screen is lit.

## 7. Power

None of this can be measured without the board and a meter. What changed:

| change | when it helps | expected |
|---|---|---|
| backlight and panel pins held through deep sleep (the hold was never really on) | poweroff, suspend | up to the backlight's ~45 mA, if the lamp leaked on |
| the LED's data pin held low through deep sleep | poweroff, suspend | a WS2812 latched on by noise |
| the amplifier's shutdown pin driven through light sleep | screen dark, idle | the amp's idle current, if its pin floated on |
| the USB console's reader no longer wakes every second | idle, screen dark | one light-sleep wake a second fewer |
| status line sent only when it changes | screen lit | 59 of 60 SPI bursts and DMA wakes |
| writers woken only when their request fits | music, games | ~6× fewer cross-core wakes (each one 80→240 MHz) |
| no I2S restart after a song | after every sound | 145 ms of I2S and amp-off output |
| FLAC −47%, MP3 −11-15%, mixer −50% | music | that much less CPU at 240 MHz per second of sound |
| a paused or hidden clip gives back 240 MHz | video paused, behind another terminal | ~11 mA (the battery model's figure) |
| JPEG −25% a slice | video | shorter bursts at 240 MHz per frame |
| image hash skipped on wake, one PSRAM test | every wake | ~320 ms less awake per wake |

The poweroff drain reported (99% to 78% in about 10 hours, about 50 mA)
is most likely the first row. Only a meter can say for sure.

## To check on the board

Power first.

1. **Poweroff drain:** `poweroff` overnight, then compare `battery -l`
   with the last drain (99% → 78% in ~10 h). With a meter: the current
   while off, which should be well under a milliamp.
2. **Suspend:** the same with `suspend -t 3600`.
3. **Idle, screen dark:** the current, and `cpufreq`'s time at 80 MHz
   and in light sleep. The USB reader, the amp pin and the status line
   all count here.
4. **Music with the screen off:** the current while `play` runs a FLAC,
   then an MP3 (fewer wakes, cheaper decoding, mixer).
5. **Video:** the current playing and paused. `tools/video_test.py PORT
   CLIP` for frames dropped, which should be fewer or the same.
6. **Configuration by hand:** the board's `build/freenove-fnk0104b/sdkconfig`
   does not take new defaults (see CLAUDE.md), so add these lines to it,
   then `make flash`:
   ```
   CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y
   CONFIG_SPIRAM_MEMTEST=n
   CONFIG_SPI_FLASH_SUPPORT_MXIC_OPI_CHIP=n
   CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n
   CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH=y
   CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=n
   CONFIG_FREERTOS_USE_TIMERS=n
   CONFIG_ESP_EVENT_LOOP_IN_EXT_RAM=y
   CONFIG_ESP_EVENT_POST_FROM_IRAM_ISR=n
   CONFIG_NVS_ALLOCATE_CACHE_IN_SPIRAM=y
   ```
   The first one reaches the board only with the bootloader, which
   `make flash` writes.
7. **Memory:** `free` after boot with Wi-Fi up, and again with a few
   terminals and a pipeline. Internal free and the low mark should be
   about 40 KB higher. PSRAM's total should match the base (the program
   is back to 23 pages).
8. **Boot and wake time:** the first `dmesg` lines and how long until
   the prompt, after a power-on and after `suspend -t 60`. Expect about
   250 ms and 70 ms sooner.
9. **Timing-sensitive things now in PSRAM:**
   - `nes -v` should still run at 60 frames a second.
   - Music while copying files to / and to the card, with no gap.
   - A clip with its sound.
10. **Re-time FLAC:** HACKING.md says 11×, 3× and 1.5× real time for
    44.1/16, 96/24 and 192/24. QEMU says about 1.9× faster now, so time
    it and update those figures.
11. **The board suites:** `make hwtest progtest scripttest langtest`,
    then `make stress SECONDS=240`.
12. **The real build:** `make size` (with the registry) and `make` from
    the top of the project, with the applications APPS adds.

## Proposals

These would help, but each changes something a user could notice, or
needs the board to decide:

- **Light sleep while the screen is lit:** most of a lit minute is idle
  between keys. The backlight's PWM would have to keep running through
  it (LEDC on the RC_FAST clock), and the renderer would have to wake on
  time.
- **A lower CPU ceiling with the screen dark:** 80-160 MHz instead of
  80-240. Music decodes far within it now. A clip or a game would lift it.
- **The keyboard watched by the ULP while dark:** the main core would
  only wake for a key, as in suspend.
- **Larger mixer DMA blocks while the screen is dark:** fewer interrupts
  per second, at the cost of latency.
- **The clip sender's scanline wait:** a timer instead of a busy poll.
- **Read-ahead on the SD card** in a thread of its own, for music and
  clips: fewer stalls at 240 MHz.
- **The terminal renderer:** send only the changed spans when scrolling
  (scroll diffing), or double-buffer.
- **LittleFS:** a larger cache, for faster reads of `/`.
- **The codec's ADC:** powered down whenever nothing records.
- **The modem:** close an open data session before poweroff. Today
  `modem_power_off` skips the modem entirely while in data mode.
- **NES:** `nes6502_execute` in IRAM (it is the emulator's hot loop).
- **The renderer's pixel buffer:** internal when there is room.
- **The mixer's rings** are a second of int32 stereo at the stream's
  rate: 352 KB at 44.1 kHz, 1.5 MB at 192 kHz. Keeping 16-bit sources
  as int16, or a half-second ring, would halve that.
- **The TLS certificate bundle:** the common set instead of the full one
  (the full one is 67 KB of the program). A site whose CA is missing
  would fail.
- **Flash chip drivers:** the drivers for flash chips the boards do not
  have could go once `esptool flash-id` names the board's chip. They
  sit in IRAM; `libspi_flash` has 9.5 KB there in all.
