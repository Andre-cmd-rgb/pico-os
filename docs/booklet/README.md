# The pocket booklets

Two little printed booklets, small enough to keep with the machine:

- **`guide.pdf`**, the pocket guide: what PocketType is, first steps, the
  keys, the shell, power and the battery, notes, the diary, music, video,
  games, the internet and files, looks, what to do when something goes
  wrong, and every command. Sixteen pages with a contents page.
- **`pico.pdf`**, the pico language: from the first program to games and
  web requests, with the programs that are in `~/pico` on the machine, and
  a reference on the back. Sixteen pages.

Each is two A4 sheets. Every sheet folds into an eight-page booklet of its
own -- part 1 and part 2 -- so the four sheets make four small booklets,
about 74 by 105 mm each.

`make` prints the PDFs from the HTML with Chromium; `make check` says
whether every page still fits after an edit. The pages are written in
reading order in `guide.html` and `pico.html`; `zine.js` lays them out for
the fold, numbers them and makes the contents page, and `zine.css` is the
look of both. The screenshots in `img/` are the machine's own
(`screenshot`, then `tools/xfer.py pull`), in the amber theme, enlarged
four times with square pixels so that no printer blurs them; the notes,
calendar and to-do shown are made up for the purpose.

## Printing

On **one side** of the paper -- no front and back -- A4, **landscape**,
at **actual size** (100%, not "fit to page"). Colour looks best, but it
reads fine in black and white.

## Folding one sheet into a booklet

1. Fold it in half top to bottom, crease it, and open it again.
2. Fold it in half left to right, then each half in half again, so that
   the creases mark out the eight pages; open it.
3. Fold it in half left to right and cut along the dashed line, from the
   fold to the next crease: only through the middle two pages.
4. Open it, fold it top to bottom with the print outside, hold the two ends
   and push them together: the cut opens into a cross. Keep going until the
   pages lie flat against each other, then fold them round so that the
   cover is at the front.

The top row of each sheet is printed upside down on purpose: folded, it
comes out the right way up.
