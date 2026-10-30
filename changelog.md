# Changelog

## [1.0.1] - 2026-10-30

### Fixed
- `CMakeLists.txt`: `REQUIRES` uváděl neexistující komponentu `esp_log` (komponenta
  logu se v ESP-IDF jmenuje `log`) → čistý ESP-IDF build (`idf.py`) selhal při
  konfiguraci na „Failed to resolve component“. Opraveno na `log`. Kód beze změny,
  PlatformIO build se netýká.

## [1.0.0] - 2026-10-20

### Added
- Počáteční verze: obecná fronta logovacích záznamů (`INFO`/`WARN`/`ERROR`/
  `SUCCESS`, časová značka = uptime v sekundách) + latchovaný stavový automat
  pro poruchy.
- Až 32 nezávisle sledovaných poruch (`uint32_t` bitmapa), každá se dvěma
  nezávislými osami stavu: aktivní/vyřešeno × potvrzeno/nepotvrzeno —
  standardní tvar průmyslového alarm/event logu.
- `unacked` bit se nastavuje jen při čerstvém přechodu neaktivní → aktivní,
  nikdy při doznění poruchy — `ack()` tak umí ztišit i pořád trvající
  poruchu; záznam zmizí až po vyřešení **a** potvrzení zároveň. Porucha, která
  se po potvrzení vrátí, prochází celým cyklem znovu.
- Volitelná zpoždění na poruchu `FaultConfig(debounceMs, clearDelayMs)`:
  on-delay (podmínka musí trvat, než se porucha stane aktivní — filtruje
  krátké výpadky) a off-delay (podmínka musí zmizet na danou dobu, než se
  aktivní porucha zruší — filtruje „cvakající“ podmínku, která by jinak při
  každém návratu založila nový nepotvrzený cyklus). Žádné zpoždění nikdy
  nepotvrzuje ani neztišuje. Měřeno monotónním 64bitovým `esp_timer` (odolné
  vůči driftu FreeRTOS ticku pod DFS/`esp_pm`, bez přetečení). Žádný
  samostatný `tick()` — `reportFault()` se volá periodicky s aktuální
  syrovou hodnotou.
- `onFaultChange(bit, active)` callback při každém přechodu (po on/off-delay),
  volaný mimo interní zámek (z lokální kopie pořízené pod zámkem — bez data
  race při souběžné výměně callbacku). Knihovna sama nerozhoduje o bzučáku/
  LED/logování — to je politika vrstvy nad ní.
- Log fronta při zaplnění zahodí nový záznam, nikdy neblokuje volajícího;
  zahozené záznamy počítá `droppedLogCount()`.
- `log()` má `printf` format atribut — nesoulad formátu a argumentů je
  varování při překladu.
- Thread-safe — log metody přes FreeRTOS frontu (N zapisovatelů / 1 čtenář),
  metody poruch přes staticky alokovaný mutex (nemůže selhat na OOM)
  s omezeným čekáním (`MUTEX_WAIT_MS` = 50 ms) a bezpečným výsledkem při
  timeoutu místo neomezeného blokování.
- Neplatný index poruchy (`>= MAX_FAULTS`), timeout mutexu a selhání
  `begin()` se hlásí přes `ESP_LOGE`, nikdy se tiše neignorují.
- `begin()` je idempotentní, vrací `ESP_ERR_INVALID_ARG` pro nulovou kapacitu
  a `ESP_ERR_NO_MEM` při nedostatku paměti; poruchy fungují i bez něj.
- Třída je nekopírovatelná (vlastní mutex a frontu).
- Zdokumentovaná omezení: každou poruchu hlásit z jednoho tasku, žádné volání
  z ISR, zpráva logu max. 47 bajtů.
- ESP-IDF balení (`CMakeLists.txt`, `idf_component.yml`), licence MIT.

Ověřeno na HW (ESP32-P4): výpadek spojení s 60s on-delay (krátké výpadky pod
limitem poruchu nevyhlásí), porucha senzoru, potvrzení aktivní i vyřešené
poruchy, opětovný výskyt, callback s logováním, vyprazdňování logu do UI.
Off-delay ověřen jen překladem a čtením kódu (žádná porucha ho v testovacím
projektu nepoužívá).
