# Third-party components

MonoMesh uses the following third-party resources. The map data is not
distributed with the repository: it is generated locally by the user.

## OpenStreetMap data

The maps produced by the project tools are derived from
**© OpenStreetMap contributors**, available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/).
The attribution is written automatically into the metadata of the `.mmap`
containers and must remain visible wherever the data (or derived works such as
tiles) is redistributed. `.osm.pbf` files are not included in the repository:
every user must obtain them from an ODbL-compatible provider (e.g. Geofabrik).

## "Toner" map style

The Toner style rasterized by the tools is an interpretation of the **Toner**
theme by Stamen Design. Attribution: *Map tiles by Stamen Design, under
CC BY 3.0. Data by OpenStreetMap, under ODbL.*

## Roboto Condensed Bold (clock glyphs)

The e-paper clock glyphs embedded in `src/clock_font_large.h` and
`src/clock_font_small.h` are rasterized (by `tools/gen_clock_font.py`) from
**Roboto Condensed Bold**, Copyright 2011 Google Inc., licensed under the
[Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0).
The embedded rasterizations are derived data from that font and are
redistributed under the same license. Regenerating the headers with a different
TTF (`--font <path>`) changes the font those files derive from, and the notice
must be updated accordingly.

## Meshtastic

MonoMesh is an independent implementation interoperable with the
[Meshtastic](https://meshtastic.org/) wire protocol; the official firmware is
distributed under the GNU GPL v3. "Meshtastic" is a trademark of Meshtastic
LLC; this project is neither affiliated with nor endorsed by Meshtastic LLC.
