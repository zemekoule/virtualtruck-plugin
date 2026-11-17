# Changelog – virtualtruck

Verze se při startu hry zapisuje do `game.log.txt` (`virtualtruck 2.0.0: writing to …`, do verze 1.9.0 `drivelog …`) a do `data/events.jsonl` (řádek `start`).

## 2.0.0 – 2026-11-17

- Plugin přejmenovaný z `drivelog` na `virtualtruck`: soubor `virtualtruck.so`, prefix `virtualtruck` v `game.log.txt`. Starý `drivelog.so` je potřeba z adresáře `plugins` hry smazat, jinak poběží oba.
- Data zapisuje do `data/` vedle repa (`virtualtruck/data/<hra>/`), formát se nemění.

## 1.9.0 – 2026-11-15

- Stopa navíc zapisuje průměrnou spotřebu podle palubního počítače (`fuel_consumption`, l/km, `null` když hra hodnotu neposílá).
- Konfigurace vozu navíc obsahuje SPZ (`license.plate`) a zemi SPZ (`license.plate.country`).

## 1.8.0 – 2026-11-13

- Konfigurace vozu (řádek `truck` a pole `truck` u herních událostí) navíc obsahuje objem nádrže `fuel.capacity` (litry), pro dojezd na plnou nádrž ve statistikách.

## 1.7.1 – 2026-11-12

- Oprava: `bus_job_id` se měnil během zakázky, protože hra do konfigurace `bus_job` doplňuje skutečné časy odjezdu a příjezdu (`source_stop.departure_time.actual`, `destination_stop.arrival_time.actual`) a mění `bus.stages.current`. Tyto atributy se do identity zakázky nepočítají.

## 1.7.0 – 2026-11-12

- Autobusy (ETS2, DLC autobusů): zapisuje konfiguraci `bus_job` (řádek `bus_job` s `bus_job_id`) a připojuje ji k herním událostem v polích `bus_job` / `bus_job_id`, stejně jako `car_job`. Zakázku ukončí `bus_job.completed`. Událost zrušení zatím neznáme, SDK 1.16 autobusové události nepopisuje. Nedokončené zakázky drží `bus_job.state`.

## 1.6.0 – 2026-10-25

- Data každé hry zvlášť: `data/<game_id>/` podle identifikace hry ze SDK (`ats`, `eut2` pro ETS2), takže jde použít i v ETS2. Dosavadní `data/*` patří do `data/ats/`.
- `job.state` / `car_job.state` drží všechny nedokončené zakázky (dva řádky na zakázku: identifikátor a identita). Externí zakázka pozastavená kvůli interní dostane po obnovení původní identifikátor. Soubor zmizí, když nezbude žádná nedokončená zakázka.

## 1.5.0 – 2026-10-25

- Identifikátor zakázky: konfigurace zakázky (`job`, `car_job`) má pole `job_id` / `car_job_id` (skutečný čas převzetí v sekundách, jako řetězec), herní události ho mají u přiložené zakázky. Zůstává při změně `cargo.loaded` (nakládka do vlastního návěsu) i po restartu hry uprostřed zakázky – drží se v `data/job.state` / `data/car_job.state`, po doručení nebo zrušení se soubor smaže.
- Návěs: konfigurace návěsů (`trailer.0` … `trailer.9`, jen značka, název, typ nástavby, náprava, SPZ, id, příslušenství nákladu) se zapisuje řádkem `trailer` s `index` při změně a připojuje k herním událostem v poli `trailers` (jen existující návěsy, první je zapřažený).
- Stopa navíc zapisuje poškození nákladu (`cargo_damage`) a opotřebení kamionu (`wear_engine`, `wear_transmission`, `wear_cabin`, `wear_chassis`, `wear_wheels`) a prvního návěsu (`trailer_wear_body`, `trailer_wear_chassis`, `trailer_wear_wheels`), vše 0,0–1,0, `null` když hra hodnotu neposílá.

## 1.4.0 – 2026-10-25

- Stopa navíc zapisuje `trailer_connected` (první návěs připojen, `true`/`false`, `null` když hra hodnotu neposílá).

## 1.3.0 – 2026-10-25

- Osobní doprava: zapisuje konfiguraci `car_job` (řádek `car_job` v `events.jsonl`) a připojuje ji k herním událostem v poli `car_job`, stejně jako `job` u kamionových zakázek.

## 1.2.0 – 2026-10-24

- Stopa (`trace.jsonl`) navíc zapisuje rychlostní limit z navigace (`speed_limit`, m/s), čas do odpočinku (`rest_stop`) a do povinné přestávky (`mandatory_break`, herní minuty). Když hra hodnotu neposílá, je `null`.
- Při startu zapisuje do `events.jsonl` řádek `start` s verzí pluginu.
- Oprava: řádky zapsané dřív, než hra pošle herní čas (konfigurace po startu), mají `game_time: null` místo `0`.
- Oprava: herní události mimo zakázku (např. pokuta po doručení) už nemají připsanou předchozí zakázku.

## 1.1.0 – 2026-10-24

- Zapisuje kamion: řádek `truck` při změně (značka, model, id) a pole `truck` u každé herní události.

## 1.0.0 – 2026-10-24

- První verze: převzetí zakázky (`job`), herní události (doručení, zrušení, pokuty, mýto, trajekt, vlak) a stopa každých 5 s jízdy (pozice, rychlost, palivo, tachometr).
