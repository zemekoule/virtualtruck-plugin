# virtualtruck plugin

Telemetrický plugin pro American Truck Simulator a Euro Truck Simulator 2 (macOS). Zaznamenává jízdy pro aplikaci virtualtruck.

Zapisuje do `../data/<hra>/` (`ats`, `eut2`), tedy do složky `data` vedle repa:

- `events.jsonl` – převzetí zakázky, doručení / zrušení, pokuty, mýto, trajekty, vlaky,
- `trace.jsonl` – každých 5 s jízdy (mimo pauzu) pozice, rychlost, palivo, tachometr.

Cesta k `data/` se do pluginu vloží při překladu. Po přesunu složky je potřeba plugin znovu přeložit a nainstalovat. Co plugin ze SDK čte, je v `SDK.md`, změny v `CHANGELOG.md`.

## Build

Potřebuje Xcode Command Line Tools (`clang++`, `make`).

```sh
make
```

Výsledkem je `virtualtruck.so` (x86_64 – hra na Macu běží přes Rosettu).

## Instalace

1. Ukonči hru.
2. ```sh
   make install
   ```
   Zkopíruje `virtualtruck.so` do `American Truck Simulator.app/Contents/MacOS/plugins/` ve Steam instalaci.

   Pokud skončí `Operation not permitted`: macOS (Správa aplikací) nedovolí terminálu zapisovat do balíku `.app`. Buď terminálu povol *Nastavení systému → Soukromí a zabezpečení → Správa aplikací*, nebo `virtualtruck.so` přetáhni s ⌥ ve Finderu:
   ```sh
   open . ~/Library/Application\ Support/Steam/steamapps/common/American\ Truck\ Simulator/American\ Truck\ Simulator.app/Contents/MacOS/plugins
   ```

   ETS2: `make install` umí jen ATS, do ETS2 přetáhni `virtualtruck.so` s ⌥ ve Finderu:
   ```sh
   open . ~/Library/Application\ Support/Steam/steamapps/common/Euro\ Truck\ Simulator\ 2/Euro\ Truck\ Simulator\ 2.app/Contents/MacOS/plugins
   ```
3. Pokud je v adresáři `plugins` ještě `drivelog.so` (verze do 1.9.0), smaž ho.
4. Spusť hru. Pokud hra upozorní na SDK pluginy, potvrď.
5. Kontrola: vznikne `data/<hra>/events.jsonl` a `data/<hra>/trace.jsonl`. V `game.log.txt` hry je řádek `virtualtruck <verze>: writing to …` – verze musí odpovídat poslední v `CHANGELOG.md`.

## Odinstalace

Ukonči hru a spusť `make uninstall`. Data v `data/` zůstanou.

## Aktualizace

Ukonči hru, pak `make install` (přeloží a přepíše). Za běhu hry jde plugin vyměnit i příkazem `sdk reload` v herní konzoli.

## Licence

MIT, viz `LICENSE`. `scs_sdk_1_16/` je SCS Telemetry SDK 1.16 s vlastní licencí v `scs_sdk_1_16/sdk_license.txt`.
