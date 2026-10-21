# `monomesh-tiles` — generatore tile nativo (C)

Implementazione nativa, senza dipendenze Python, dello stesso generatore di tile
Toner usato per la MicroSD del M5Paper Mono. Fa esattamente quello che fa
`tools/mmap_converter.py`, ma con un binario singolo, multithread e ~4–10×
più veloce (fino a ~100× sull'estrazione da PBF).

## Build

```bash
./tools/native/build.sh            # binario statico in tools/native/monomesh-tiles
./tools/native/build.sh --debug    # build non ottimizzata
CC=gcc ./tools/native/build.sh     # con un compilatore di sistema
```

Il build script usa, in ordine: `$ZIG`, `zig` nel `PATH`, il toolchain
self-contained in `tools/.toolchain/` (Zig 0.13, nessun root richiesto), oppure
`$CC`/`cc`. Con Zig il binario è **statico (musl)**: un singolo file eseguibile
senza dipendenze runtime. zlib è compilata dai sorgenti vendorizzati in
`third_party/zlib` (licenza zlib).

## Uso

```bash
# area intorno a una città (elenco offline) oppure bounding box esplicito
tools/native/monomesh-tiles --pbf nord-ovest-260912.osm.pbf --lat 45.8081 --lon 9.0852 \
    --radius_km 8 --zooms 12,13,14 --output ./tiles --jobs 14

tools/native/monomesh-tiles --pbf nord-ovest-260912.osm.pbf \
    --bbox 45.51 8.92 45.88 9.44 --zooms 12,13,14 --output ./tiles

tools/native/monomesh-tiles --tui        # interfaccia interattiva (come lo script Python)
tools/native/monomesh-tiles --help
```

Opzioni principali: `--dark` (stile invertito), `--png-mode fast|small|mono`
(`mono` = PNG 1-bit, ideale per e-paper), `--force` (ignora cache e tile
esistenti), `--jobs N` (thread di rendering), `--extract-jobs N` (thread di
parsing PBF), `--cache-dir DIR`.

Le tile vengono scritte in `<output>/<z>/<x>/<y>.png`, pronte da copiare in
`/sdcard/tiles/` sul M5Paper Mono.

## Cache condivisa con la versione Python

Il formato della cache (`.pbf_cache/<area>_<crc32 bbox>.mosmb`) è **identico** a
quello dello script Python: la chiave è il CRC32 dei quattro double del bbox,
quindi i due strumenti si scambiano la cache senza conversioni. Il binario
scrive una cache leggermente più compatta perché scarta già in estrazione le
classi di highway che lo stile non disegna (footway, cycleway, steps, …).

La **versione 2** del formato aggiunge la popolazione delle località; le cache
in formato 1 vengono riconosciute come obsolete e rigenerate automaticamente
(servono per disegnare i laghi, che prima non c'erano).

## Stile dipendente dallo zoom

A basso zoom lo stile di riferimento disegna pochissimo: mostrare tutte le
classi di strade a z9 riempiva la tile di nero (fino al 94 % di inchiostro).
Le regole condivise da Python e C sono:

| zoom | strade | acqua | etichette |
| ---: | :--- | :--- | :--- |
| ≤ 7 | motorway, trunk (+link) | laghi (relation), niente ruscelli | city, max 3 |
| 8–9 | + primary (+link), spessori 1–2 px | + grandi fiumi solo da z10 | city, max 6 |
| 10–11 | + secondary (+link) | + fiumi e canali | + town, max 9 |
| 12 | + tertiary, unclassified | + aree d'acqua piccole | max 14 |
| 13 | + residential, living_street | + ruscelli, fossi, drenaggi | + village, suburb |
| 14 | + service, track, pedestrian | tutte | + hamlet, neighbourhood |
| ≥ 15 | + path | tutte | max 20 |

Inoltre:

- **laghi**: le relation `type=multipolygon` con `natural=water`/`water`/
  `waterway=riverbank` vengono assemblate in anelli (esterni + interni) e
  riempite; gli anelli interni diventano isole bianche. Senza questo Lago di
  Como, Maggiore, Lugano, Iseo e Garda non comparivano affatto;
- **niente puntini**: i poligoni d'acqua sotto i 5 px (z9) / 3 px (z10-11) /
  2 px (z12) vengono scartati, quindi non si vedono più gli 846 laghetti da
  1 pixel che sporcavano le tile a basso zoom;
- **etichette**: ordinate per popolazione (tag `population`, con fallback per
  tipo), con un tetto per tile e controllo di sovrapposizione, come fa lo stile
  di riferimento.

## Prestazioni misurate

Hardware: 14 core. Dataset: `nord-ovest-260912.osm.pbf` (563 MB).

| Carico | Script originale | `mmap_converter.py` ottimizzato | Binario C |
| :--- | ---: | ---: | ---: |
| Mariano Comense, 843 tile z12–14, a freddo (estrazione + render) | ~22.5 min | 44.1 s | **10.8 s** |
| Stessa area, cache calda (solo render) | ~21.4 min | 3.0 s | **0.72 s** |
| Intera regione nord-ovest, 20 188 tile z12–13 | ~8.5 h | 25.8 s | **16.5 s** (incl. estrazione) |
| Area r50km (Mariano Comense), 296 349 tile z9–17 | — | ore | **131 s** (estrazione + render) |

Fasi del caso 843 tile con il binario: estrazione 10.0 s (14 thread),
indice 0.44 s, rendering 0.24 s (~3 400 tile/s).

## Fedeltà grafica

Il rasterizzatore è stato calibrato su Pillow: le linee di larghezza 1 usano
Bresenham (pixel-identiche a Pillow), quelle più larghe sono bande con giunzioni
rotonde; i testi usano un atlas di glifi pre-renderizzato con gli stessi font
(DejaVu) e le stesse dimensioni dello script Python.

Verifica automatica (`tools/tests/test_native.py`):

- l'estrazione produce **esattamente** gli stessi elementi dello script Python
  (stesse way, stesse coordinate, stesse etichette) sul dataset di test;
- su una mappa densa, l'immagine è identica per **> 97 %** dei pixel, con le
  differenze confinate ai bordi di 1 px delle strade (nessuna differenza
  strutturale: stesso numero di tile, stessi simboli, stesse etichette).

Lo script Python mantiene anche i test di stile: laghi da relation, filtro per
zoom, scarto dei corpi d'acqua microscopici e politica delle etichette.

## Test

```bash
python3 -m unittest discover -s tools/tests -v     # 19 test (Python + nativo)
```

I test nativi compilano automaticamente il binario se manca; se non è
disponibile alcun compilatore vengono saltati.
