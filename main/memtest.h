#pragma once

/* Power-on self test of the PSRAM, logged to dmesg; takes well under 0.1 s. */
void memtest_quick(void);
