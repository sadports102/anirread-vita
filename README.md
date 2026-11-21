# Aniread

A manga and novel reader for the PS Vita, made to match the look of aniwatch.

Aniread reads manga and light novels from your memory card. It can also find and download manga from MangaDex, and it remembers where you stopped in every book.

> Version 0.1.0 · Homebrew for PS Vita (needs HENkaku / Ensō)

---

## Features

**Library**
- Manga and Novels tabs, with a rotating banner and cover grids
- Details screen with the chapter or volume list (D-pad Left/Right jumps 10)
- Progress bars and "N% READ" on every title, plus resume
- Covers and info come from a `meta/` folder, or get fetched online
- EPUBs without a cover use the cover from inside the book

**Online (MangaDex)**
- Popular, Latest and Search (with the Vita keyboard)
- Download one chapter, a range of chapters or all of them
- Content filter and Data Saver settings

**Manga reader**
- JPG, PNG, WEBP, BMP and GIF pages
- Landscape and portrait
- Fit to screen or to width, plus 2x zoom
- Overlay with a seek bar and a clock
- Can go on to the next chapter by itself

**Novel reader**
- EPUB (pictures and the book's own contents list included) and TXT (UTF-8, UTF-16 or Windows-1252, with chapter detection)
- Reader menu: themes Black, Dark Grey, Sepia and White; fonts Outfit and Literata; 6 text sizes; line spacing; margins; justified text
- Contents list for jumping to a chapter
- Landscape and portrait
- Turn pages with buttons, the sticks, taps or swipes
- Your place in the book is saved as a percentage, so it survives font and size changes
- PDF isn't supported. Convert PDFs to EPUB with [Calibre](https://calibre-ebook.com/)

---

## Library layout

Everything lives in `ux0:data/aniread/`:

```
ux0:data/aniread/
├── manga/
│   └── <Title>/
│       ├── meta/            cover.jpg, banner.jpg (optional), info.txt
│       └── chapters/
│           ├── chapter001/  page images, sorted by name
│           └── chapter002/
├── novels/
│   ├── Some Book.epub       loose files work
│   ├── Some Book.jpg        (optional cover for a loose file)
│   └── <Series>/            or one folder per series
│       ├── meta/
│       ├── Volume 1.epub
│       └── Volume 2.epub
├── progress.txt
└── settings.txt
```

### info.txt

Every line is optional:

```
title=My Series
year=2021
genres=Fantasy, Adventure
synopsis=A short description.
rating=8.4
type=MANGA
status=Ongoing
```

Use `type=NOVEL` for novels.

---

## Controls

**Library**

| Button | Action |
|---|---|
| D-pad / sticks | Move |
| X | Open / select |
| O | Back |
| L / R | Switch tabs |
| START | Side menu |
| Triangle | Rescan library |
| Square | Search / download a range / fetch info |
| Touch | Tap anything |

**Manga reader**

| Button | Action |
|---|---|
| Left / Right, L / R | Previous / next page |
| X | Show or hide the overlay |
| Triangle | 2x zoom |
| Square | Fit to screen or width |
| O | Back |

**Novel reader**

| Button | Action |
|---|---|
| Left / Right, L / R, stick | Previous / next page |
| Tap the left or right side, or swipe | Previous / next page |
| X / tap the middle | Show or hide the overlay |
| Triangle / Square | Bigger / smaller text |
| START | Contents |
| SELECT | Reader menu |
| O | Back |

In portrait mode the D-pad is turned to match the screen.

---

## Settings

Auto next chapter, Orientation (Landscape or Portrait), Resume (Ask or Always), Page fit, Data Saver, Content filter and Clear history.

The novel reader menu options are saved in `settings.txt` too.

---

## Building

You need [VitaSDK](https://vitasdk.org/) with SDL2, SDL2_ttf, SDL2_image, libcurl, OpenSSL and zlib (all from vdpm).

```
./build.sh
```

This makes `build/aniread.vpk`. Install it with VitaShell.

### Fonts

These go in `res/`:
- **Outfit** (`font.ttf`, `font_bold.ttf`) for the interface
- **Literata** (`literata.ttf`, `literata_bold.ttf`, `literata_italic.ttf`) for the novel reader. Literata is optional; without it the reader uses Outfit.

Both fonts are under the SIL Open Font License.

### Troubleshooting

If the build stops with `Cannot allocate N bytes for SCE data`, make the `vita_pad` array at the end of `main.c` bigger (4096, 8192, 12288 and so on).

---

## Planned

- Audiobook player (MP3, then M4B/M4A)
- Webtoon (vertical scroll) mode
- Downloads queue screen
- Auto-sorting from an `import/` folder
- Text-to-speech
- LiveArea artwork

---

## Credits

- Manga data and downloads: [MangaDex](https://mangadex.org/). Aniread isn't affiliated with MangaDex. Please support the scanlation groups and the official releases.
- Metadata: [Kitsu](https://kitsu.io/)
- Built with SDL2, libcurl, OpenSSL, zlib and jsmn
- Interface style shared with aniwatch

## License

MIT License

Copyright (c) 2026 sadports102

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
