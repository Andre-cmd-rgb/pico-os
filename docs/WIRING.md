# Wiring

PocketType is built for one board at a time. The board file sets every pin,
so on a supported board there is nothing to configure:

```sh
make boards                         # what there is
make BOARD=freenove-fnk0104b flash  # the default
```

`make menuconfig` still shows every pin under **PocketType → Device
drivers**; the board file only supplies the defaults.

| Board | State |
|---|---|
| `freenove-fnk0104b` | Freenove ESP32-S3 Display, 2.8" 240x320 IPS. Everything is soldered on: display, SD slot, codec, LED, charger. |
| `devkit-uno-shield` | A plain S3 DevKitC with a 2.4" ILI9341 Uno shield. **Never got a picture out of the panel** — see the end of this file. |

---

# Freenove ESP32-S3 Display (FNK0104B)

One USB-C port, which is the S3's own USB: console, flashing and power all
come through it. There is no serial bridge chip, so the console is USB
Serial/JTAG (the board file already sets that).

## What is on the board

Nothing here needs wiring; it is here so you can check a pin against the
firmware. Every line was taken from Freenove's own sketches and setup files
for this board (`Freenove_ESP32_S3_Display`, the `FNK0104AB` branches).

| Part | Signal | GPIO |
|---|---|---|
| ILI9341 panel, SPI | CS | 10 |
| | MOSI | 11 |
| | SCK | 12 |
| | MISO | 13 |
| | DC (data/command) | 46 |
| | backlight (PWM, on = high) | 45 |
| | reset | the board's own reset line |
| microSD slot, SDMMC 4-bit | CLK | 38 |
| | CMD | 40 |
| | D0 | 39 |
| | D1 | 41 |
| | D2 | 48 |
| | D3 | 47 |
| ES8311 codec | I2S MCLK | 4 |
| | I2S BCLK | 5 |
| | I2S LRCK (word select) | 7 |
| | I2S DOUT (to the speaker) | 8 |
| | I2S DIN (from the microphone) | 6 |
| | amplifier enable | 1 |
| | I2C SDA / SCL (address 0x18) | 16 / 15 |
| Battery | voltage, through a 1:2 divider | 9 |
| WS2812 RGB LED | data | 42 |
| BOOT button | | 0 |

The panel is wired portrait (240x320). The board file rotates it to
landscape, which gives a 53x24 character console; `PT_LCD_ROTATION` turns
it back. It also picks the colour settings Freenove uses for this panel:
BGR order, inverted, and the alternative start-up table
(`PT_LCD_INIT_ALT`) — panels sold as ILI9341 are not all the same silicon,
and this one wants lower drive and its own gamma curves.

## What you plug in

### Battery

A single lithium cell in the 2-pin socket on the back. The board charges it
from USB. **Check the polarity marked next to the socket before plugging
anything in** — cells from different sellers use the same connector with the
wires swapped, and getting it wrong kills the charger.

The firmware reads the cell every five seconds:

```
$ battery -c 2500     # once, when a cell is fitted: what it holds
$ battery
3.92 V  71%  on battery, about 14h 10m left
$ battery -w          # keep watching
$ power               # among the rest of the power state
```

The board cannot measure the current, so the percentage is not read
straight off the voltage, which sags under load and stands high on the
charger. The firmware estimates the current from what the board is doing
(backlight, CPU, radio) or from the charger's set 300 mA, learns the
cell's resistance from how far the voltage steps when the charger or the
backlight changes, puts the sag back, and counts the charge between
readings. Tell it the capacity with `battery -c` when you fit a cell:
that also starts the learning again, for the new one.

Below 3.5 V it prints a warning on the screen once a minute. At 3.3 V it
syncs the filesystems, unmounts the card and goes into deep sleep, so the
cell is never run flat. Both levels are in menuconfig
(`PT_BATTERY_WARN_MV`, `PT_BATTERY_OFF_MV`).

One thing the voltage cannot tell you on this board: the divider hangs off
the charger's output, so with USB plugged in and **no cell at all** it
still reads about 4.1 V, exactly like a full one. The warning and the
cut-off are unaffected -- those only happen on a cell that is really
running down -- but do not read "4.09 V" as proof that a battery is
fitted. (Below 2.5 V the firmware does say "no battery fitted": that is a pin with
nothing driving it, which is what a board without this charger looks
like.)

### Speaker

An 8 Ω speaker in the speaker socket. The amplifier on the board is only
switched on while something is playing. The speaker control reaches the
codec's full output range at 100%. Check that the speaker has room to move
inside the case: contact with the case can cause rattling and distortion.

```
$ beep                 # 1 kHz for 200 ms
$ beep 440 1000        # concert A for a second
$ volume 70
$ play tune.wav        # 16-bit WAV, mono or stereo, any rate 8-48 kHz
$ rec -t 5 note.wav    # five seconds from the microphone
$ cat tune.raw > /dev/audio    # raw 16-bit mono at the current rate
```

`/dev/audio` is the plain interface: write samples to it and they play, read
from it and you get the microphone. The microphone is the small hole next to
the codec; `rec -g 30` turns its gain up (0 to 42 dB).

### Headphone jack

The board has no jack, but the four free pins of the IO header (below)
take one: a PCM5102A DAC module (the purple GY-PCM5102, about 2 euro)
on the chip's second I2S controller. It gets the mix in stereo and in 32
bits, the DAC's widest, with the volume applied on the way into those 32
bits, so a quiet setting loses none of the sound's detail.
`CONFIG_PT_AUDIO_JACK` in menuconfig (Device drivers, Sound) turns the
driver on; the pins are the defaults there.

| PCM5102A | Board | |
|---|---|---|
| VIN | 3V3 | the module makes its own 1.8 V and analogue supply |
| GND | GND | |
| BCK | GPIO 2 | the bit clock, 64 times the rate |
| LCK | GPIO 3 | left/right |
| DIN | GPIO 14 | the samples |
| SCK | GND | no master clock: the DAC makes its own from BCK (on the purple board, bridge the SCK pads on the back) |
| FLT, DEMP, FMT | GND | normal filter, no de-emphasis, I2S (pads H1L, H2L and H4L on the back to L) |
| XSMT | 3V3 | not muted (pad H3L to H) |

Keep BCK, LCK and DIN short, with the ground wire running alongside
them: they carry a 2.8 MHz clock, and a long loop picks up the Wi-Fi.

A socket with a switch says when a plug is in: wire the switch between
**GPIO 21** and GND. menuconfig's **The plug switch** says which kind it
is: one that closes when a plug goes in (the default), one that opens,
or one that closes to 3.3 V.

The purple module's own socket has one that opens, but it is not a
switch of its own: of its five legs, two touch only while the socket is
empty (a meter beeps across them then, not with a plug in), and one of
those two is **the left channel's spring**, which rests on the other
until a plug lifts it. Wire **only the other one, the rest contact, to
GPIO 21** -- nothing to GND -- and choose "opens": with the socket empty
the DAC's left output holds the pin low through the spring. Ground
either leg and the left ear is silent. The spring is the leg with a
track to the module's circuit; if in doubt, wire one to GPIO 21 and
play a sound panned left: the wrong one silences the left ear.

Then sound goes to the headphones while a plug is in and back to the
speaker when it comes out (`dmesg`: `audio: headphones in`, `out`), and
`volume speaker` keeps it on the speaker; with such a socket `volume
jack` is refused, because the jack would play into the pin that tells,
unless the build allows it for trying out (menuconfig: **Let `volume
jack` keep sound on the jack with no plug**). Without a plug switch,
`volume jack` and `volume speaker` choose. `/etc/power` keeps the
choice. It is one or the other, never both, and an alarm always rings
on the speaker.

The DAC plays at line level, 2.1 V RMS at full scale, which is far too
loud for headphones, so the jack has a volume of its own, kept apart
from the speaker's: it starts at 40%, and each percent is 0.6 dB (50% is
30 dB under full scale). `volume` sets the one that is playing.

The PCM5102A's output is made for a line input (1 kΩ or more). Low-impedance
headphones work -- Sennheiser's HD 450SE are 18 Ω and need only tens of
millivolts, so it runs nowhere near its limit -- but they draw more
current from it than it is designed for, and loud passages distort
first. For the cleanest sound put a small headphone amplifier after it,
or use headphones of 100 Ω or more. Some of these boards have a 470 Ω
resistor in series with each output (look for two `471` parts by the
socket): then headphones come out quiet, and want the volume near the top.

### microSD card

Straight into the slot, FAT32. It mounts on `/mnt/sd`, four bits wide.
`umount /mnt/sd` before pulling it out, or use `sync` first.

```
$ df
$ ls /mnt/sd
```

### CardKB

M5Stack's CardKB goes on the I2C pins, **SDA 16 / SCL 15** — the same two
wires as the codec. That matters for how you power it: the codec is a 3.3 V
part, so power the keyboard from **3V3**, not 5 V. A CardKB powered from 5 V
pulls the bus up to 5 V, which is outside what the codec is rated for. If it
does not answer at 3.3 V, use a level shifter; don't feed it 5 V.

| CardKB wire | Board |
|---|---|
| black | GND |
| red | **3V3** |
| yellow (SDA) | GPIO16 |
| white (SCL) | GPIO15 |

It can be plugged and unplugged while running; `dmesg` shows `cardkb:
keyboard found`.

### USB keyboard

The board has one USB port and it is the console, so a USB keyboard would
need that port. Use the CardKB or the PC's keyboard here. (The USB host
driver is still in the build for the DevKitC, which has two ports.)

### SIM module

The four-pin header brings out **TX 43 / RX 44**, 5 V and ground, which is
where a module that speaks AT commands goes: SIM800, SIM7600, A7670,
EC200. Cross the data lines -- the board's TX to the module's RX.

| Board | Module |
|---|---|
| GPIO43 (TX) | RXD |
| GPIO44 (RX) | TXD |
| GND | GND |
| 5V | **see below** |

**Do not power the module from the board.** A GSM module pulls up to 2 A
for a couple of milliseconds every time it talks to a tower; USB through
this board will brown out and reset the chip in the middle of sending a
message. Give the module its own supply (a lithium cell, or a 5 V supply
that can do 2 A) and join the grounds. A 1000 µF capacitor across the
module's supply is the usual fix for the last of the dips.

The firmware looks for the module for half a minute after boot, without
holding anything else up, because modules take their time to start:

```
$ dmesg | grep modem
[   11.4] modem: SIMCOM_SIM7600G-H on TX 43 / RX 44, imei 86...
$ modem
module   SIMCOM_SIM7600G-H
sim      READY
network  registered on vodafone IT
signal   -71 dBm
data     down
$ sms send +393331234567 hello from my own computer
$ sms                    # * marks unread
$ modem data on          # mobile internet, over PPP
$ ping -c 3 1.1.1.1
```

The APN belongs to your network and is set in menuconfig
(`PT_MODEM_APN`); the wrong one dials fine and routes nothing. `modem at
<command>` sends anything through by hand, for instance `modem at +CSQ`.
Calls are not supported.

#### The SIM800L

The small red SIM800L board is the cheap one, and it differs from the
rest in two ways that can each kill it:

- **It runs on 3.4 to 4.4 V, not 5 V.** Its VCC goes to the cell: to
  OUT+ and OUT- of the charger module, the same two wires the board's
  battery lead comes from, with a 1000 µF capacitor (6.3 V or more,
  minus to ground) soldered across VCC and GND at the module. Never the
  header's 5 V pin.
- **Its serial lines are 2.8 V.** What goes into its RXD is brought down
  from the board's 3.3 V with two resistors; what comes out of its TXD
  the board reads as it is.

| SIM800L | Goes to |
|---|---|
| VCC | the cell's + (charger module OUT+), capacitor to GND |
| GND | the cell's - (OUT-) **and** the header's GND |
| RXD | GPIO43 through 1 kΩ, and 2.2 kΩ from RXD to GND |
| TXD | GPIO44 |
| RST, RING, DTR | not connected |

Screw the antenna on before it is powered. It takes a micro-SIM, whose
PIN must be turned off first (in a phone): the driver does not enter
one. It is **2G only** -- GSM and GPRS -- so it registers only where the
network still runs 2G; mobile data works, at tens of kilobits a second.

**It needs a SIM with a 2G part**, and many new SIMs have none: they are
USIMs only, whose 2G directory holds the SIM's identity but not the
files a 2G phone needs on the network (the cipher key EF_Kc, the BCCH
list, the phase). A phone reaches 2G through the USIM and works; the
SIM800 reads such a SIM as ready, then, three seconds after its radio
starts, puts it aside as "SIM wrong" (CME 15) without the network saying
anything. A Lyca SIM of 2026 was one. `modem` says so when it sees it
("it has no 2G part"); `modem diagnose radio-off` shows the SIM staying
ready for as long as the radio stays off. A 4G module (A7670E) reads
USIMs.
It draws 15 to 20 mA all the time, the board asleep or not, which
empties the cell in about a week: a switch in its VCC wire is worth it.
The blue "SIM800L v2" board has a regulator of its own and wants 5 V at
2 A, from its own supply, never from the header.

#### The ESP-800L

A SIM800L on a board the shape of an ESP-01, sold as "SIM800L ESP-800L
core board, pin compatible ESP8266, 5V": a 2x4 header with no labels, a
diode marked M1 on its supply (5 V in, about 4.3 V to the SIM800L), a
micro-SIM holder underneath and an IPEX socket for the antenna. The
pins that matter are the header's four corners, where an ESP-01 has
them; find them with a meter on its beep setting, the board unpowered:

- **GND** beeps to the antenna socket's outer ring;
- **VCC** is the corner diagonally opposite, and beeps (or shows half a
  volt on the diode setting) to one end of M1;
- **RXD** is the far end of GND's row, **TXD** the far end of VCC's.

| ESP-800L | Board |
|---|---|
| GND | GND |
| VCC | 5V -- with USB in; see below for the cell |
| RXD | GPIO43 (TX), through 1 kΩ |
| TXD | GPIO44 (RX) |

The printed antenna beside the module is not the one to count on: an
antenna on the IPEX socket is. `modem` says `signal none` until it hears
a network; `modem scan` lists the networks it hears. A module that
restarts every half a minute (`dmesg` says "modem: the module has
started" again and again) is short of current when it transmits: a
1000 µF capacitor across VCC and GND at the module, short thick wires,
and for good, M1 bridged with solder and VCC from the cell (charger
module OUT+) -- after which it must never see 5 V again.

The data service's name is set on the board, not in the build:
`modem apn data.lycamobile.it lmit plus` for Lyca in Italy, say; `modem
ussd *123#` asks the network for the credit.

On the cell's wires the module draws whatever the board is doing. The
driver lets it sleep whenever the port is quiet (AT+CSCLK=2: about
1.5 mA registered, and a message or a call still wakes it), `modem off`
turns its radio off and lets it sleep (under 1 mA; awake it would be
about 15) until `modem on`, and `poweroff` or a flat cell do the same.
`battery` counts what it draws.

### Wi-Fi

No wiring: the radio is in the chip and its antenna is on the board.

```
$ wifi connect           # pick from a list; the password is asked for
$ wifi                   # signal, address, gateway
$ ntp                    # the clock, which otherwise starts in 1970
```

A network that connects is saved in `/etc/wifi` and joined again at every
boot, about six seconds in. `wifi off` shuts the radio down and hands back
about 30 KB of internal RAM; `wifi on` brings it back and rejoins.

### Expanded IO header

**GPIO 2, 3, 14 and 21** are not used by anything on the board. On the
non-touch version **17 and 18** are free as well; on the touch version they
are the touch controller's interrupt and reset.

## First power-on

This is what the first boot on a real FNK0104B printed, with a cell, a
speaker and a card attached:

```
[  0.107] battery: 4.09 V (92%) on GPIO9
[  0.181] wifi: radio up (802.11 b/g/n)
[  0.513] lcd: ili9341 (alt init) on SPI2 at 40 MHz, 320x240
[  0.579] es8311: codec at 0x18, 16000 Hz
[  0.580] audio: 16000 Hz mono, speaker and microphone
[  0.704] rootfs: / is littlefs on flash:storage, 13240 KB free of 13248 KB
[  1.079] init: 80 programs, starting shell
```

1. `make flash` — the board is `/dev/ttyACM*`. If it does not appear, hold
   **BOOT**, tap **RESET**, release **BOOT**, and flash again.
2. `make term`. The boot log ends at `user@pockettype:~$`.
3. `dmesg` should have lines for `lcd`, `rootfs`, `es8311` (with a battery
   line if a cell is fitted, and `sd` if a card is in).
4. `lcdtest` draws eight colour bars: red, green, blue, yellow, magenta,
   cyan, white, black. `backlight 20` dims the screen.
5. In a development build (menuconfig, System: **Development build**),
   `i2cdetect` should list `0x18  ES8311 audio codec`, plus `0x5f` with a
   CardKB attached.
6. `beep`, then `rec -t 3 /tmp/t.wav && play /tmp/t.wav`.
7. `keytest` (a development build too) if a CardKB is attached; press q
   three times to quit.

## When something looks wrong

| Symptom | Try |
|---|---|
| Nothing on the screen, backlight on | `dmesg \| grep lcd`; `backlight 100`; then `lcdtest` |
| Red and blue swapped | menuconfig → **Panel expects BGR** |
| Looks like a photo negative | **Invert colours** |
| Upside down | **Rotation** 3 instead of 1 |
| `es8311: no codec` | `i2cdetect` (a development build): nothing at all means the bus is held down, probably by whatever is on the keyboard header — unplug it and reboot |
| No sound, but the codec is found | the amplifier's pin is a shutdown input, not an enable: toggle **The amplifier runs when that pin is low** in menuconfig |
| Sound is distorted | check the speaker's clearance and mounting in the case; lower `volume` if the speaker or amplifier is overdriven |
| Screen lights up but stays blank | menuconfig → **Start-up sequence**: try the standard table instead of the alternative |
| Noise or torn lines on the screen | the panel runs at 40 MHz here, which is over its datasheet: a development build's `lcdtest verify` reads frames back from it; drop **Bus clock** to 26000000 if they come back wrong |
| `sd: no usable card` | reseat it; try another card; `PT_SD_MMC_D1` = -1 (one-wire mode) if the card is flaky |
| `battery` says "no battery fitted" with a cell fitted | check the socket polarity and that the cell is not below its protection cut-out |
| `cardkb: keyboard disconnected` again and again | the bus is shared with the codec on weak internal pull-ups: shorten the cable, or add 4.7 kΩ from SDA and SCL to 3V3 |
| `rec` gives an I/O error | the microphone is clocked by the transmitter; if this comes back, the transmitter failed to start |

---

# ESP32-S3 DevKitC with a 2.4" Uno shield (`devkit-uno-shield`)

This one is kept because the code paths are still there (the 8-bit parallel
bus, `lcdprobe`, `lcdreg`, SD over SPI), but **the panel never displayed
anything**: the controller answers on the bus, the start-up sequence goes
through, `lcdprobe` sees every data line move, and the screen stays lit and
blank. Treat the wiring below as unverified.

## Display

The shield wires the ILI9341's 8 data lines to Uno pins D8, D9 and D2–D7.
Shields print either name next to the pin: the data bit (**LCD_D0**…**LCD_D7**)
or the Uno pin (**D8**, **D9**, **D2**…**D7**). They are not the same numbers:
LCD_D0 is Uno pin 8, LCD_D2 is Uno pin 2.

| Shield label | Uno pin | Signal | ESP32-S3 | menuconfig |
|---|---|---|---|---|
| LCD_D0 | D8 | data bit 0 | GPIO4 | `PT_LCD_D0` |
| LCD_D1 | D9 | data bit 1 | GPIO5 | `PT_LCD_D1` |
| LCD_D2 | D2 | data bit 2 | GPIO6 | `PT_LCD_D2` |
| LCD_D3 | D3 | data bit 3 | GPIO7 | `PT_LCD_D3` |
| LCD_D4 | D4 | data bit 4 | GPIO15 | `PT_LCD_D4` |
| LCD_D5 | D5 | data bit 5 | GPIO16 | `PT_LCD_D5` |
| LCD_D6 | D6 | data bit 6 | GPIO17 | `PT_LCD_D6` |
| LCD_D7 | D7 | data bit 7 | GPIO18 | `PT_LCD_D7` |

| Shield label | Uno pin | Signal | ESP32-S3 | menuconfig |
|---|---|---|---|---|
| LCD_RS | A2 | data/command | GPIO1 | `PT_LCD_RS` |
| LCD_CS | A3 | chip select | GPIO2 | `PT_LCD_CS` |
| LCD_WR | A1 | write strobe | GPIO42 | `PT_LCD_WR` |
| LCD_RST | A4 | reset | GPIO41 | `PT_LCD_RST` |
| LCD_RD | A0 | read strobe | GPIO10, or **3V3** | `PT_LCD_RD` |
| 5V, 3.3V, GND | | power | 5V, 3V3, GND | |

RD must never float: the controller drives the data lines when it is low.
Either tie it to 3V3 or give it a GPIO the firmware holds high — and if it
shares a pin with anything else (the SD card's CS, say), that other driver
will take the pin back and the screen goes blank again.

## SD card slot and CardKB

| Shield pin | Signal | ESP32-S3 | menuconfig |
|---|---|---|---|
| D10 | CS | GPIO10 | `PT_SD_CS` |
| D11 | MOSI | GPIO11 | `PT_SD_MOSI` |
| D13 | SCK | GPIO12 | `PT_SD_SCK` |
| D12 | MISO | GPIO13 | `PT_SD_MISO` |

CardKB goes on SDA GPIO3 / SCL GPIO0 there. Those two are the only pins the
low-power core can read I2C on, which is what lets a key wake the board from
`suspend`.

## Diagnosing a blank parallel panel

`lcdprobe` drives every pin by hand and reads the controller's ID; `lcdreg`
sends commands and reads registers one at a time (both in a development
build). Both take the pins away
from the display bus until the next reboot, so reboot after using them.
The one thing that did change behaviour during that hunt is now in the
driver for good: a 2 ms pause after every start-up command, because the
controller ignores "sleep out" when commands arrive back to back.
