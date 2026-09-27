# Board files

Each file here turns on the drivers a board has and sets their pins, the
way `arch/*/configs/*_defconfig` does in the Linux kernel. Every driver can
also be switched on by itself in `make menuconfig` under
*PocketType → Device drivers*; a board file is only a starting set.

```sh
make BOARD=freenove-fnk0104b       # the default
make BOARD=devkit-uno-shield flash
make menuconfig                    # change anything on top of the board file
```

The build lands in `build/<board>/`, so several boards can be built side by
side without clobbering each other.

A board file only *seeds* a configuration. Once `build/<board>/sdkconfig`
exists it is the truth, and editing the board file changes nothing until:

```sh
make BOARD=freenove-fnk0104b defconfig   # start again from the board file
```

which throws away whatever `menuconfig` changed. `make build` says so when
the board file is the newer of the two.

| Board | What it is |
|---|---|
| `freenove-fnk0104b` | Freenove FNK0104A/B, ESP32-S3R8 with a 2.8" ILI9341, SDMMC card slot, ES8311 audio, battery charger |
| `devkit-uno-shield` | An ESP32-S3 devkit wired to a 2.4" Uno ILI9341 shield (12 jumper wires; see docs/WIRING.md) |
| `waveshare-esp32-p4-wifi6` | Waveshare ESP32-P4-WIFI6: ESP32-P4, microSD, ES8311 audio, USB OTG, Wi-Fi through an ESP32-C6; no screen. Builds, not yet run |
| `qemu` | No hardware: for `make test` |

To add a board, copy the closest file, change the pins, and list it above.
Each file names its chip (`CONFIG_IDF_TARGET`); the chip's own settings are
in `sdkconfig.defaults.<chip>`.
