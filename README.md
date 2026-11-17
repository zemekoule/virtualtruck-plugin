# virtualtruck plugin

Telemetrický plugin pro American Truck Simulator a Euro Truck Simulator 2 (macOS). Zaznamenává jízdy pro aplikaci virtualtruck.

Zapisuje do `~/Library/Application Support/virtualtruck/data/<hra>/` (`ats`, `eut2`):

- `events.jsonl` – převzetí zakázky, doručení / zrušení, pokuty, mýto, trajekty, vlaky,
- `trace.jsonl` – každých 5 s jízdy (mimo pauzu) pozice, rychlost, palivo, tachometr.

Co plugin ze SDK čte, je v `SDK.md`, změny v `CHANGELOG.md`.

## Odesílání na server

S konfigurací `~/Library/Application Support/virtualtruck/config` plugin za běhu hry posílá oba soubory na server aplikace [virtualtruck-app](https://github.com/zemekoule/virtualtruck-app):

```
url=http://localhost:8100
token=<osobní token z aplikace>
```

Bez konfigurace jen zapisuje soubory. Odesílá každých 10 s ve vlastním vlákně, vždy nejdřív události, pak stopu. Soubory jsou zároveň fronta: když server neběží, data zůstanou v souborech a odejdou při dalším spojení (za běhu hry), do `upload.log` vedle konfigurace se zapíše jen první chyba a obnovení. Když server řádky odmítne (např. pod stejným číslem řádku má jiný obsah, nebo má víc řádků než soubor), odesílání toho souboru do dalšího startu hry zastaví a odpověď serveru zapíše do `upload.log`.

## Build

Potřebuje Xcode Command Line Tools (`clang++`, `make`).

```sh
make
```

Výsledkem je `virtualtruck.so` (x86_64 – hra na Macu běží přes Rosettu), linkuje systémový `libcurl`.

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
5. Kontrola: vznikne `~/Library/Application Support/virtualtruck/data/<hra>/events.jsonl` a `trace.jsonl`. V `game.log.txt` hry je řádek `virtualtruck <verze>: writing to …` – verze musí odpovídat poslední v `CHANGELOG.md` – a `virtualtruck: uploading to …`, nebo `… not uploading`, když chybí konfigurace.

## Odinstalace

Ukonči hru a spusť `make uninstall`. Data a konfigurace v `~/Library/Application Support/virtualtruck/` zůstanou.

## Aktualizace

Z verze 2.x (data v `data/` vedle repa): ukonči hru a před instalací přesuň data na nové místo, jinak plugin začne nové soubory a zakázky rozjeté přes aktualizaci ztratí identifikátor:

```sh
mkdir -p ~/Library/Application\ Support/virtualtruck
mv ../data ~/Library/Application\ Support/virtualtruck/data
```

Ukonči hru, pak `make install` (přeloží a přepíše). Za běhu hry jde plugin vyměnit i příkazem `sdk reload` v herní konzoli.

## Licence

MIT, viz `LICENSE`. `scs_sdk_1_16/` je SCS Telemetry SDK 1.16 s vlastní licencí v `scs_sdk_1_16/sdk_license.txt`.
