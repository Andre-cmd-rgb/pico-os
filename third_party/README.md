# Other people's code

Two components, each somebody else's work, kept apart from the rest of
the tree and built without its warning flags. Nothing else in PocketType
comes from outside.

| Component | From | Licence | Why not ours |
|---|---|---|---|
| `minimp3/` | github.com/lieff/minimp3 | CC0 (public domain) | MPEG layer III's Huffman and window tables *are* the ISO standard, and a decoder that is slightly wrong sounds worse than one that refuses the file. FLAC and WAV are written here (`codec/`). |
| `nofrendo/` | Retro-Go's fork of Matthew Conte's Nofrendo | **GPL v2** | A NES emulator is a 6502, a picture unit, an audio unit and sixty cartridge mappers. Andre asked for a port rather than a rewrite. |

`nofrendo/port/rg_system.h` is ours: the core expects one header from its
usual host (Retro-Go) for logging and a CRC, and that file provides it so
the rest of the core is byte for byte what upstream ships. The other half
of the port -- screen, sound, joypad -- is in `emu/`.

**On the GPL:** linking Nofrendo in makes a *distributed* PocketType
binary GPL v2. Building it for yourself changes nothing. Turn
`PT_NES` off in menuconfig and it is not linked at all.

To update either one, download the new files over the old and rebuild;
there are no local changes to carry over.
