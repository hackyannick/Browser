# Kite – ein moderner Webbrowser für Windows 2000

<img src="docs/kite-icon.png" alt="Kite-Icon" align="right" width="48">

Kite ist ein Webbrowser mit **eigener, in C++ geschriebener Rendering-Engine**,
der unter **Windows 2000** (und allen späteren Windows-Versionen) läuft. Er
bringt das heutige Web – HTTPS mit TLS 1.2, modernes CSS mit Flexbox, Grid,
CSS-Variablen und SVG – auf ein Betriebssystem aus dem Jahr 2000, ohne auf
den Internet Explorer oder Systembibliotheken angewiesen zu sein.

![Startseite](docs/screenshots/startseite.png)

## Funktionen

**Browser**

- Tabs (Strg+T, Strg+W, Strg+Tab), Adressleiste mit Suche, Zurück/Vor, Startseite
- Lesezeichen (Strg+D), Verlauf, Zoom (Strg +/−/0, Strg+Mausrad)
- Suchen auf der Seite mit Hervorhebung (Strg+F, F3)
- Formulare: Textfelder, Passwörter, Textbereiche, Checkboxen, Radiobuttons,
  Auswahllisten, Absenden per GET und POST
- Text markieren und kopieren, Kontextmenü (Link in neuem Tab, Link kopieren,
  Bild speichern …)
- Downloads mit „Speichern unter“, Seitenquelltext (Strg+U), Seite speichern
- Cookies (dauerhaft gespeichert), HTTP-Proxy, einstellbare Suchmaschine
- JavaScript (abschaltbar unter Extras → Einstellungen), JavaScript-Konsole
  (Strg+Umschalt+J), `alert`/`confirm`/`prompt` als Windows-Dialoge
- Portabel: Einstellungen liegen in `kite.ini` neben `kite.exe`

**Engine („Kite Engine“)**

| Bereich | Umfang |
|---|---|
| HTML | HTML5-Tokenizer und Tree-Builder mit Fehlerkorrektur, Zeichenreferenzen, Zeichensatz-Erkennung (UTF-8, Windows-1252, ISO-8859-1/-15, UTF-16) |
| CSS | Kaskade mit Spezifität und `!important`, Selektoren bis Level 4 (`:is()`, `:where()`, `:not()`, `:nth-child()`, Attributselektoren …), `@media` (inkl. Bereichs-Syntax), `@supports`, `@import`, `@layer`, CSS-Verschachtelung, Custom Properties (`var()`), `calc()`/`min()`/`max()`/`clamp()`, `::before`/`::after` |
| Layout | Block- und Inline-Formatierung mit Zeilenumbruch, Margin-Collapsing, Floats und `clear`, Tabellen (colspan/rowspan, automatische Spaltenbreiten), **Flexbox**, **Grid** (Spalten, `repeat()`, `fr`, `minmax()`, `auto-fill`), relative und absolute Positionierung (`fixed` vereinfacht), `overflow`-Clipping, Listen |
| Grafik | Hintergründe und Hintergrundbilder, Rahmen (inkl. klassischem 3D-Look), abgerundete Ecken, `box-shadow`, Transparenz, `translate`-Transformationen, PNG/JPEG/GIF/BMP/**WebP** (auch `<picture>`), **SVG** (Inline und als Bild, mit Kantenglättung) |
| Schriften | **Webfonts** (`@font-face`, TTF/OTF/WOFF/WOFF2) werden geladen und prozesslokal installiert |
| JavaScript | **QuickJS** (ES2023) mit eigener DOM-Anbindung: `document`/`window`, Elemente, `querySelector`, `innerHTML`, `classList`, `style`, `dataset`, Events mit Bubbling und `preventDefault`, Timer, `requestAnimationFrame`, `fetch`, `XMLHttpRequest`, `localStorage`, `URL`, `document.cookie`, `getBoundingClientRect`/`getComputedStyle`, `<noscript>` |
| Netzwerk | HTTP/1.1, **TLS 1.2 (BearSSL)** mit Zertifikatsprüfung, gzip/deflate/Brotli, Weiterleitungen, Cookies, Proxy (CONNECT), `data:`- und `file:`-URLs |

![PyPI in Kite](docs/screenshots/pypi.png)

![Formular-Suche](docs/screenshots/formular-suche.png)

## Was (noch) nicht geht

- JavaScript: ES-Module (`type=module`), Web Components/Shadow DOM, Canvas,
  WebSockets, Web Workers und Medienwiedergabe fehlen. `MutationObserver` ist
  nur ein Platzhalter, `localStorage` lebt nur bis zum Schließen des Tabs.
  Große Single-Page-Anwendungen (React, Angular …) laufen daher oft nur
  teilweise.
- CSS-Animationen werden nicht abgespielt (es wird der Endzustand gezeigt),
  Rotation/Skalierung werden ignoriert; AVIF-Bilder werden nicht dekodiert,
  animierte GIF/WebP zeigen nur das erste Bild.
- TLS 1.3 und HTTP/2 werden nicht unterstützt (alle gängigen Server sprechen
  noch TLS 1.2 und HTTP/1.1).
- Windows 2000 bringt nur begrenzte Unicode-Schriften mit; Emoji und manche
  Schriftsysteme erscheinen als Kästchen.

## Installation unter Windows 2000

1. `kite.exe` und `cacert.pem` (Stammzertifikate) in denselben Ordner kopieren.
2. `kite.exe` starten. Eine Installation oder weitere DLLs sind nicht nötig –
   die Datei ist vollständig statisch gelinkt und benötigt nur Bibliotheken,
   die zu Windows 2000 gehören.
3. **Wichtig:** Datum und Uhrzeit des Systems müssen stimmen, sonst schlägt die
   Zertifikatsprüfung bei HTTPS-Seiten fehl.

Empfohlen: Windows 2000 mit Service Pack 4 und mindestens 128 MB RAM; große,
moderne Seiten profitieren deutlich von einem schnellen Prozessor.

## Bauen

Gebaut wird mit MinGW-w64 als Cross-Compiler, z. B. unter Linux:

```sh
sudo apt install cmake g++-mingw-w64-i686    # Debian/Ubuntu
./build-win2k.sh                             # Ergebnis: dist/Kite/kite.exe
```

Unter Windows funktioniert dasselbe mit MSYS2 (Umgebung „MINGW32“):

```sh
pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake make
cmake -S . -B build-win -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

`tools/check-win2k-imports.sh` prüft nach dem Bauen, dass die EXE keine
Windows-API importiert, die es unter Windows 2000 nicht gibt. Neuere
MinGW-Laufzeiten verwenden einige Vista-Funktionen (Condition Variables,
`GetThreadId`); für diese enthält `src/win32/compat.c` Ersatzimplementierungen.

Die GitHub-Actions-Pipeline (`.github/workflows/build.yml`) baut bei jedem
Push automatisch `kite.exe` und stellt sie als Artefakt bereit.

### Engine-Tests und Werkzeuge (Linux/macOS)

Die Engine ist plattformunabhängiges C++11 und lässt sich auch nativ bauen:

```sh
cmake -S . -B build-linux && cmake --build build-linux
./build-linux/kite-tests                          # Unit-Tests
./build-linux/kite-dump --width 1024 seite.html   # Box-Baum ausgeben
./build-linux/kite-dump https://example.org --ppm vorschau.ppm
./build-linux/kite-dump --js https://example.org  # mit JavaScript
```

## Aufbau des Quellcodes

```
src/engine/            plattformunabhängige Engine (C++11)
  base/                Strings, UTF-8, Geometrie, Mutex
  html/                Tokenizer, Tree-Builder, Zeichenreferenzen
  dom/                 Dokumentmodell
  css/                 CSS-Parser, Eigenschaften, Kaskade, UA-Stylesheet
  layout/              Box-Baum, Block/Inline, Floats, Tabellen, Flex, Grid
  paint/               Display-List und Painter
  image/               PNG/JPEG/GIF (stb_image) und SVG-Rasterizer
  net/                 URL, Sockets, TLS (BearSSL), HTTP, Cookies
  script/              JavaScript: QuickJS-Anbindung (script.cpp) und
                       Web-API in JavaScript (dom_js.cpp)
  text/                Webfont-Konvertierung (WOFF/WOFF2 → TrueType)
  page/                Seite: verbindet DOM, Styles, Layout, Skripte und Painting
src/win32/             Windows-Oberfläche (Win32-API, GDI, Common Controls)
src/tools/kite_dump.cpp  Headless-Werkzeug zum Testen der Engine
tests/                 Unit-Tests
third_party/           BearSSL (MIT), QuickJS (MIT), libwebp (BSD), Brotli (MIT),
                       stb_image (Public Domain)
resources/cacert.pem   Mozilla-Stammzertifikate (MPL 2.0)
```

Die Engine zeichnet in eine plattformneutrale Display-List; das
Windows-Frontend rastert diese in eine 32-Bit-DIB (Rechtecke, Bilder und SVGs
mit Alphablending in Software, Text über GDI). So braucht Kite weder GDI+ noch
AlphaBlend und läuft auf einer unveränderten Windows-2000-Installation.

## Lizenzen der Fremdkomponenten

- [BearSSL](https://bearssl.org) © Thomas Pornin – MIT-Lizenz
  (`third_party/bearssl/LICENSE.txt`)
- [QuickJS](https://bellard.org/quickjs/) © Fabrice Bellard und Charlie Gordon
  – MIT-Lizenz (`third_party/quickjs/LICENSE`)
- [libwebp](https://chromium.googlesource.com/webm/libwebp) © Google – BSD-Lizenz
  (`third_party/libwebp/COPYING`, `PATENTS`)
- [Brotli](https://github.com/google/brotli) © Google – MIT-Lizenz
  (`third_party/brotli/LICENSE`)
- [stb_image](https://github.com/nothings/stb) von Sean Barrett – Public Domain
- Stammzertifikate aus dem Mozilla-CA-Programm – Mozilla Public License 2.0
