#!/usr/bin/env python3
"""bin/tags.c and bin/lrc.c, on the host, under ASan/UBSan.

Tags: files ffmpeg writes -- FLAC with Vorbis comments and a PICTURE,
MP3 with ID3v2.4 (UTF-8) and ID3v2.3 (UTF-16), ID3v1 alone, WAV -- read
back field by field, the cover byte for byte and the lyrics as the text
that went in. Then a few thousand damaged copies, which must only ever
come out empty. LRC: exact times for stamps, offsets, a chorus stamped
twice, timed words, plain lyrics, and junk.
"""
from pathlib import Path
import random
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

TITLE = "Café del Mar — ñandú"
ARTIST = "Ünïcode Ärtist"
ALBUM = "Àlbum: 東京"
LYRICS = ("[00:01.00]Prima riga, già\n[00:04.50]Second line ♪\n"
          "[00:09.20]<00:09.20>Red <00:09.70>carpet <00:10.30>tonight\n")


def build(tmp):
    exe = tmp / "tags_test"
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(ROOT / "kernel/include"), str(ROOT / "tools/tags_test.c"),
                    str(ROOT / "bin/tags.c"), str(ROOT / "bin/lrc.c"), "-o", str(exe)],
                   check=True)
    return str(exe)


def tags(exe, path):
    out = subprocess.run([exe, str(path)], check=True, capture_output=True, text=True,
                         timeout=20).stdout
    return dict(line.split(" ", 1) if " " in line else (line, "") for line in out.splitlines())


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *map(str, args)], check=True)


def duration_ms(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                          "-of", "csv=p=0", str(path)], check=True, capture_output=True,
                         text=True).stdout
    return float(out) * 1000


def synchsafe(n):
    return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])


def unsync(data):
    """ID3 unsynchronisation: a 0x00 after every 0xff that could pass for a sync."""
    out = bytearray()
    for i, b in enumerate(data):
        out.append(b)
        if b == 0xff and (i + 1 == len(data) or data[i + 1] >= 0xe0 or data[i + 1] == 0):
            out.append(0)
    return bytes(out)


def id3(version, enc, jpeg, unsync_cover=False):
    """A tag written by hand: real USLT and APIC frames, text in `enc`."""
    codec = {0: "latin-1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}[enc]
    nul = b"\0\0" if enc in (1, 2) else b"\0"

    def frame(fid, body, flags=0):
        size = synchsafe(len(body)) if version == 4 else len(body).to_bytes(4, "big")
        return fid.encode() + size + bytes([0, flags]) + body

    def text(fid, value):
        return frame(fid, bytes([enc]) + value.encode(codec))
    frames = b"".join((text("TIT2", TITLE), text("TPE1", ARTIST), text("TALB", ALBUM),
                       text("TPE2", "Various"), text("TRCK", "3/12"), text("TPOS", "1/2"),
                       text("TDRC" if version == 4 else "TYER", "2024")))
    frames += frame("USLT", bytes([enc]) + b"eng" + "".encode(codec) + nul + LYRICS.encode(codec))
    picture = b"\0image/jpeg\0\x03\0" + jpeg
    frames += frame("APIC", unsync(picture), 0x02) if unsync_cover else frame("APIC", picture)
    frames += bytes(64)                                 # padding
    return b"ID3" + bytes([version, 0, 0]) + synchsafe(len(frames)) + frames


def check_file(exe, path, name, cover=None, lyrics=True, text=True, rate=44100, bits=0,
               slack=120):
    t = tags(exe, path)
    assert t["status"] == "0", (name, t)
    if text:
        for key, want in (("title", TITLE), ("artist", ARTIST), ("album", ALBUM)):
            assert t[key] == want, f"{name}: {key} is {t[key]!r}"
        assert t["album_artist"] == "Various", (name, t["album_artist"])
        assert (t["track"], t["disc"], t["year"]) == ("3", "1", "2024"), (name, t)
    assert int(t["rate"]) == rate and t["channels"] == "2", (name, t)
    assert int(t["bits"]) == bits, (name, t["bits"])
    assert abs(int(t["ms"]) - duration_ms(path)) <= slack, (name, t["ms"], duration_ms(path))
    got_cover = Path(str(path) + ".cover")
    if cover:
        assert got_cover.read_bytes() == cover.read_bytes(), f"{name}: the cover differs"
    else:
        assert not got_cover.exists(), f"{name}: a cover from nowhere"
    got_lyrics = Path(str(path) + ".lyrics")
    if lyrics:
        assert got_lyrics.read_text("utf-8").strip() == LYRICS.strip(), \
            f"{name}: the lyrics differ: {got_lyrics.read_text('utf-8')!r}"
    print(f"PASS {name}")


def tag_files(exe, tmp):
    tone = "sine=frequency=440:sample_rate=44100:duration=6"
    stereo = ["-ac", "2"]
    cover = tmp / "cover.jpg"
    ffmpeg("-f", "lavfi", "-i", "testsrc2=s=600x600:d=1", "-frames:v", "1", "-q:v", "3", cover)
    meta = ["-metadata", f"title={TITLE}", "-metadata", f"artist={ARTIST}",
            "-metadata", f"album={ALBUM}", "-metadata", "album_artist=Various",
            "-metadata", "track=3/12", "-metadata", "disc=1/2", "-metadata", "date=2024-05-01",
            "-metadata", f"lyrics={LYRICS}"]

    flac = tmp / "song.flac"
    ffmpeg("-f", "lavfi", "-i", tone, "-i", cover, "-map", "0:a", "-map", "1:v", *stereo,
           "-sample_fmt", "s32", "-bits_per_raw_sample", "24", "-c:v", "copy",
           "-disposition:v", "attached_pic", *meta, flac)
    check_file(exe, flac, "FLAC: Vorbis comments, a PICTURE, LRC lyrics", cover, bits=24, slack=2)

    for version in (4, 3):
        mp3 = tmp / f"song-v2{version}.mp3"
        ffmpeg("-f", "lavfi", "-i", tone, "-i", cover, "-map", "0:a", "-map", "1:v", *stereo,
               "-c:a", "libmp3lame", "-b:a", "128k", "-c:v", "copy", "-id3v2_version",
               str(version), "-metadata:s:v", "comment=Cover (front)", *meta, mp3)
        check_file(exe, mp3, f"MP3: ffmpeg's ID3v2.{version}, APIC, lyrics in TXXX", cover)

    bare = tmp / "bare.mp3"
    ffmpeg("-f", "lavfi", "-i", tone, *stereo, "-c:a", "libmp3lame", "-b:a", "128k",
           "-id3v2_version", "0", bare)
    for version, enc, what in ((4, 3, "UTF-8, an unsynchronised cover"), (3, 1, "UTF-16")):
        mp3 = tmp / f"hand-v2{version}.mp3"
        mp3.write_bytes(id3(version, enc, cover.read_bytes(), unsync_cover=version == 4)
                        + bare.read_bytes())
        check_file(exe, mp3, f"MP3: ID3v2.{version} by hand, {what}, USLT", cover)

    v1 = tmp / "song-v1.mp3"
    ffmpeg("-f", "lavfi", "-i", tone, *stereo, "-c:a", "libmp3lame", "-b:a", "96k",
           "-id3v2_version", "0", "-write_xing", "0", v1)
    # ffmpeg will not write ID3v1 here, so by hand: 128 bytes at the end, track 7 in v1.1
    tag = (b"TAG" + b"Plain Title".ljust(30, b" ") + b"Plain Artist".ljust(30, b"\0")
           + b"Some Album".ljust(30, b" ") + b"1999" + bytes(28) + b"\0\x07\xff")
    assert len(tag) == 128
    v1.write_bytes(v1.read_bytes() + tag)
    t = tags(exe, v1)
    assert (t["title"], t["artist"], t["album"], t["year"], t["track"], t["kbps"]) == (
        "Plain Title", "Plain Artist", "Some Album", "1999", "7", "96"), t
    check_file(exe, v1, "MP3: ID3v1 alone, no Xing", lyrics=False, text=False)

    wav = tmp / "song.wav"
    ffmpeg("-f", "lavfi", "-i", tone, *stereo, "-c:a", "pcm_s16le", wav)
    check_file(exe, wav, "WAV", lyrics=False, text=False, bits=16, slack=2)

    junk = tmp / "notes.mp3"
    junk.write_text("not music at all, just some words in a file\n" * 20)
    assert tags(exe, junk)["status"] == "-22"
    print("PASS a text file is not music")
    return [flac, tmp / "song-v24.mp3", tmp / "hand-v24.mp3", tmp / "hand-v23.mp3", v1, wav]


def damaged(exe, tmp, files):
    """Truncated and scrambled copies: whatever comes out, never a crash."""
    rng = random.Random(5)
    count = 0
    for src in files:
        data = src.read_bytes()[:200_000]
        for i in range(120):
            b = bytearray(data[:rng.randrange(4, len(data))] if i % 3 == 0 else data)
            for _ in range(rng.randrange(1, 40)):
                at = rng.randrange(min(len(b), 4096 if i % 2 else len(b)))
                b[at] = rng.randrange(256)
            path = tmp / "damaged.bin"
            path.write_bytes(bytes(b))
            for suffix in (".cover", ".lyrics"):
                Path(str(path) + suffix).unlink(missing_ok=True)
            subprocess.run([exe, str(path)], check=True, capture_output=True, timeout=20)
            count += 1
    print(f"PASS {count} damaged files read without a fault")


def lrc(exe, tmp, text, moments, name):
    path = tmp / "lyrics.lrc"
    path.write_bytes(text.encode() if isinstance(text, str) else text)
    out = subprocess.run([exe, "lrc", str(path), *map(str, moments)], check=True,
                         capture_output=True, text=True, timeout=20).stdout.splitlines()
    head = out[0].split()
    return int(head[1]), out[1:]


def lrc_cases(exe, tmp):
    synced, out = lrc(exe, tmp, (
        "[ti:Song]\n[ar:Someone]\n[offset:+500]\n"
        "[00:01.00]First line\n"
        "[00:05.50][01:00.00]Chorus line\n"
        "[00:10.00]\n"
        "[00:12.00]Third  line   with   spaces\n"), [-5000, 0, 999, 12345, 61000, 9999999],
        "offset")
    assert synced == 1 and out == [
        "500 5000 First line",
        "5000 9500 Chorus line",
        "11500 59500 Third line with spaces",
        "59500 67500 Chorus line",
        "at -5000 -1 -2", "at 0 -1 -2", "at 999 0 -1", "at 12345 2 -1",
        "at 61000 3 -1", "at 9999999 3 -1"], out
    print("PASS stamps, an offset, a chorus stamped twice, a gap ending a line")

    synced, out = lrc(exe, tmp, "[00:02.00]<00:02.00>Red <00:02.50>carpet<00:03.10>tonight\n"
                      "[00:05.00]<00:05.00>Alone\n", [1800, 2000, 2400, 2900, 4000, 5100],
                      "words")
    assert synced == 1 and out == [
        "2000 5000 Red carpet tonight", "  2000 Red", "  2500 carpet", "  3100 tonight",
        "5000 13000 Alone",
        "at 1800 -1 -2", "at 2000 0 1", "at 2400 0 2", "at 2900 0 2", "at 4000 0 3",
        "at 5100 1 -1"], out
    print("PASS timed words, run together in the file, and a single one that is just the line")

    synced, out = lrc(exe, tmp, "﻿Just words\r\nwith no stamps\r\n\r\n", [0, 1000], "plain")
    assert synced == 0 and out == ["-1 -1 Just words", "-1 -1 with no stamps",
                                   "at 0 -1 -2", "at 1000 -1 -2"], out
    synced, out = lrc(exe, tmp, "[00:00.00]\nHello\nWorld\n[03:00.00]\n", [0], "wrapped")
    assert synced == 0 and out == ["-1 -1 Hello", "-1 -1 World", "at 0 -1 -2"], out
    print("PASS plain lyrics, and stamps with no words around them")

    synced, out = lrc(exe, tmp, "﻿[00:01.00]Uno\r\n[1:02.5]Due\r\n[00:61.00]bad\r\n"
                      "[aa:bb]x\r\n[00:03.00\r\n[0:4]Tre\r\n", [1000, 62600], "loose")
    assert synced == 1 and out == ["1000 4000 Uno", "4000 62500 Tre", "62500 70500 Due",
                                   "at 1000 0 -1", "at 62600 2 -1"], out
    print("PASS a byte-order mark, CRLF, short stamps; bad ones left out")

    rng = random.Random(7)
    alphabet = "[]<>:.0123456789 abc\n\t\r-+offset"
    for _ in range(400):
        junk = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 400)))
        lrc(exe, tmp, junk, [0, 5000, 99999], "fuzz")
    print("PASS 400 junk lyrics parsed without a fault")


def names(exe):
    for name, want in (("01. Central Cee - No Introduction.flac", "No Introduction"),
                       ("07 - Song.mp3", "Song"), ("1999 Song.mp3", "1999 Song"),
                       ("Artist - Title - Remix.mp3", "Title - Remix"),
                       ("/music/sub/Track.flac", "Track"), ("3) Name.wav", "Name"),
                       ("12", "12"), (".hidden", ".hidden")):
        got = subprocess.run([exe, "name", name], check=True, capture_output=True,
                             text=True).stdout.rstrip("\n")
        assert got == want, (name, got)
    print("PASS titles from file names")


def main():
    with tempfile.TemporaryDirectory(prefix="pico-tags-test-") as tmp:
        tmp = Path(tmp)
        exe = build(tmp)
        files = tag_files(exe, tmp)
        damaged(exe, tmp, files)
        lrc_cases(exe, tmp)
        names(exe)
    print("tags: FLAC, ID3v2.4/2.3/1 and WAV read back; LRC timed to the millisecond (ASan/UBSan)")


if __name__ == "__main__":
    main()
