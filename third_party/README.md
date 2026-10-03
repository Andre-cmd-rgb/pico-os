# Other people's code

Two components, each somebody else's work, kept apart from the rest of
the tree and built without its warning flags. Nothing else in PocketType
comes from outside.

| Component | From | Licence | Why not ours |
|---|---|---|---|
| `minimp3/` | github.com/lieff/minimp3 | CC0 (public domain) | MPEG layer III's Huffman and window tables *are* the ISO standard, and a decoder that is slightly wrong sounds worse than one that refuses the file. FLAC and WAV are written here (`codec/`). |
| `nofrendo/` | Retro-Go's fork of Matthew Conte's Nofrendo | **GPL v2** | A NES emulator is a 6502, a picture unit, an audio unit and sixty cartridge mappers: porting a good one was worth more than writing another. |

`nofrendo/port/rg_system.h` is ours: the core expects one header from its
usual host (Retro-Go) for logging and a CRC, and that file provides it. The
other half of the port -- screen, sound, joypad -- is in `emu/`. Two small
changes to the core itself: `nofrendo_buildpalette` gives NULL instead of
writing through an allocation that failed, and `nes/cpu.c` alone is built
with switch tables (`CMakeLists.txt`), which the SDK turns off everywhere
for code that may run with the cache off. The 6502's 256-way switch became
a tree of comparisons without them; with them a game runs 5% faster.

**On the licences:** pico-os itself is GPL v2 or later (`../LICENSE`).
Nofrendo's own files say Library GPL v2, whose text is in
`nofrendo/COPYING.LIB` (Retro-Go ships the GPL v2 as `COPYING`); that
licence lets it be used under the GPL v2 or any later version, so a
firmware image with the emulator in it, which links ESP-IDF's Apache 2.0
code too, can be passed on under GPL v3, with its source. Mapper 165
(Fire Emblem's) was left out: it was adapted from VirtuaNES, GPL v2 only.
minimp3 is public domain and asks for nothing. Turn `PT_NES` off in
menuconfig and Nofrendo is not linked at all.

To update either one, download the new files over the old and rebuild,
then put back the two changes above and `port/`.
