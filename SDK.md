# Data ze SDK telemetrie (SCS SDK 1.16)

Přehled, co SDK nabízí a co z toho plugin používá. Seznam vychází z hlaviček v `scs_sdk_1_16/include/common/`.

## Používáme

| co | od verze |
|---|---|
| kanál `game.time` | 1.0.0 |
| kanál `truck.world.placement` (jen x/y/z, natočení ne) | 1.0.0 |
| kanály `truck.speed`, `truck.fuel.amount`, `truck.odometer` | 1.0.0 |
| konfigurace `job` (celá) | 1.0.0 |
| všechny herní události (`job.*`, `car_job.*`, `player.*`) | 1.0.0 |
| konfigurace `truck` (jen `brand_id`, `brand`, `id`, `name`) | 1.1.0 |
| kanály `truck.navigation.speed.limit`, `rest.stop`, `mandatory.break` | 1.2.0 |
| konfigurace `car_job` (celá) | 1.3.0 |
| kanál `trailer.connected` (jen první návěs) | 1.4.0 |
| konfigurace `trailer.N` (jen id, značka, název, nástavba, náprava, SPZ, příslušenství nákladu) | 1.5.0 |
| kanály `job.cargo.damage`, `truck.wear.*`, `trailer.wear.*` (jen první návěs) | 1.5.0 |
| `game_id` z inicializace (složka dat podle hry) | 1.6.0 |
| konfigurace `bus_job` (celá) | 1.7.0 |
| konfigurace `truck`: `fuel.capacity` | 1.8.0 |
| kanál `truck.fuel.consumption.average` | 1.9.0 |
| konfigurace `truck`: `license.plate`, `license.plate.country` | 1.9.0 |

## Doporučeno k přidání

Postupně podle potřeby; přidané řádky přesunout do tabulky výše.

| oblast | kanály / data | k čemu |
|---|---|---|
| Navigace | `truck.navigation.distance`, `truck.navigation.time` | zbývající vzdálenost a čas, plán vs. skutečnost |
| AdBlue | `truck.adblue`, `truck.adblue.consumption.average` | spotřeba AdBlue |

## Jiné typy zakázek

- `car_job` (konfigurace + `car_job.delivered/cancelled`): osobní doprava (ATS, Ford DLC / Road Trip). Ověřeno 2026-10-24: hra posílá konfiguraci `truck` i pro auto a stopa se zapisuje; `car_job.delivered` nese `revenue`, `distance.km`, `vehicle.damage`, `cargo.damage`, `delivery.time` (absolutní herní čas, ne doba trvání), bez XP. Hodnocení kurýra a spokojenost zákazníka SDK neposílá – jsou jen v savu (`car_job_log_entry`).
- `bus_job` (konfigurace + `bus_job.completed`): autobusy v ETS2 (DLC autobusů). SDK 1.16 atributy ani události nepopisuje (jen „documented after release of bus dlc“). Viděno 2026-11-12 na 1 zakázce (plugin 1.6.0, konfiguraci ještě nezapisoval): `bus_job.completed` nese `revenue`, `earned.xp`, `passenger.satisfaction` (0–1), `distance.km` (= tachometr ve stopě) a `delivery.time` (doba trvání v herních minutách). Hra hned před touto událostí pošle konfiguraci `truck` s kamionem hráče, takže pole `truck` u ní obsahuje kamion, ne autobus. U 2. zakázky (1.7.0) už pole `truck` obsahovalo autobus, proč se to lišilo, nevíme.
  - Konfigurace `bus_job` (1.7.0, 1 linková zakázka Bratislava → Ostrava, `bus_job.market: "company_job"`): `bus.stages.count`, `bus.stages.current`, `income`, `planned_distance.km` a po úsecích (index = úsek) `source_stop.type/id/name`, `destination_stop.type/id/name` (`type` zatím jen `city`), `source_stop.departure_time.planned`, `destination_stop.arrival_time.planned` (absolutní herní čas), `source_stop.entering_passengers.planned`, `destination_stop.leaving_passengers.planned`. Při odjezdu a příjezdu hra konfiguraci pošle znovu s doplněným `source_stop.departure_time.actual` / `destination_stop.arrival_time.actual`.
  - `delivery.time` v `bus_job.completed` (186) = skutečný příjezd − skutečný odjezd, ne doba od převzetí (převzetí o 26 min dřív).
  - Zájezdová zakázka (1.7.1, 2026-11-12, Ostrava → Schönbrunn → Ostrava): `bus_job.market: "tourist_job"`, 2 úseky tam a zpět, památka má `type` `viewpoints`, na ní nikdo nevystupuje ani nenastupuje (0 / 0, plánovaní cestující jen na začátku a konci). `bus_job_id` drží celou zakázku.
  - Čekání na odjezd hra přeskočí (stopa: skok `game_time` bez pohybu a beze změny paliva): po převzetí +1079 min na plánovaný odjezd, na památce +304 min hned po příjezdu. Skutečný čas odjezdu z památky je proto v konfiguraci už od příjezdu. Pro `build.py` to není spánek.
  - Odpočinek autobusů podle uživatele netýká; hra přesto posílá `rest_stop` a `mandatory_break`. Na obou zakázkách `rest_stop` od odjezdu klesal z 660 na 539 a dál už ne, `mandatory_break` zůstal 660. Web je u autobusů nezobrazuje.
  - `delivery.time` (756) = poslední skutečný příjezd − první skutečný odjezd, včetně pobytu na památce; `distance.km` (544) = tachometr ve stopě.

## Nepoužíváme (pro deník jízd asi bez užitku)

- Obecné: `local.scale`, `multiplayer.time.offset`.
- Kamion – pohyb: `truck.local.velocity.*`, `truck.local.acceleration.*`, `truck.cabin.*`, `truck.head.offset`, natočení z `truck.world.placement`.
- Motor a převodovka: `truck.engine.rpm`, `truck.engine.gear`, `truck.displayed.gear`, `truck.hshifter.*`, `truck.engine.enabled`, `truck.electric.enabled`.
- Ovládání: `truck.input.*`, `truck.effective.*`, `truck.cruise_control`.
- Brzdy: `truck.brake.parking`, `truck.brake.motor`, `truck.brake.retarder`, `truck.brake.air.pressure*`, `truck.brake.temperature`.
- Provozní kapaliny a palubní deska: `truck.fuel.warning`, `truck.fuel.range`, `truck.adblue.warning`, `truck.oil.*`, `truck.water.*`, `truck.battery.*`, `truck.dashboard.backlight`.
- Světla a signalizace: `truck.lblinker`, `truck.rblinker`, `truck.hazard.warning`, `truck.light.*`, `truck.wipers`.
- Náprava a podvozek: `truck.differential_lock`, `truck.lift_axle*`, `truck.trailer.lift_axle*`, `truck.wheel.*`.
- Návěs – pohyb a kola: `trailer.world.placement`, `trailer.velocity.*`, `trailer.acceleration.*`, `trailer.wheel.*`.
- Konfigurace: `controls`, `hshifter`, `substances`; z `truck` vše kromě značky, modelu, id, objemu nádrže a SPZ (kola, převody, pozice kabiny…).
