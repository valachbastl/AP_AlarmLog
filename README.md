# AP_AlarmLog

Application event log + latching fault/alarm tracker for ESP-IDF. Meant to
back a status log list and an alarm/fault screen on an embedded UI (LVGL or
otherwise) — but has no UI or transport knowledge of its own.

## Features

- **Log** — a small bounded queue of human-readable status messages
  (`INFO`/`WARN`/`ERROR`/`SUCCESS`), timestamped with uptime. Any task may
  call `log()` concurrently (backed by a FreeRTOS queue). A full queue drops
  the new entry rather than blocking the caller; dropped entries are counted
  (`droppedLogCount()`). `printf`-style format strings are checked at compile
  time.
- **Faults** — up to 32 independently tracked boolean conditions (e.g.
  "connection lost", "sensor X failed"), each identified by a small bit index.
  Two independent axes of state per fault — **active/resolved** and
  **acknowledged/unacknowledged** — the standard shape of an industrial
  alarm & event log:

  ```
  OK                 --reportFault(true)-->  ACTIVE, UNACKED
  ACTIVE, UNACKED     --ack()-->              ACTIVE, ACKED
  ACTIVE, UNACKED     --reportFault(false)--> RESOLVED, UNACKED
  ACTIVE, ACKED       --reportFault(false)--> OK   (unacked bit already clear)
  RESOLVED, UNACKED   --ack()-->              OK
  ```

  A fault only re-enters `UNACKED` on a fresh inactive→active transition,
  never on the falling edge — so `ack()` silences an *ongoing* fault without
  requiring it to have cleared first, and an entry only ever disappears once
  it is both resolved and acknowledged. A fault that re-occurs after being
  acked goes through the full cycle again (re-arm).
- **On-delay / off-delay** — optional per fault: `debounceMs` (condition must
  be present this long before the fault becomes `ACTIVE` — filters glitches)
  and `clearDelayMs` (condition must be absent this long before an `ACTIVE`
  fault clears — filters a "chattering" condition that keeps dropping out,
  which would otherwise start a new unacked cycle each time). Neither delay
  ever acknowledges or silences a fault. Timed off the monotonic 64-bit
  `esp_timer` (immune to FreeRTOS tick drift under DFS/`esp_pm`, no
  wrap-around). No separate `tick()` — call `reportFault()` periodically with
  the current raw reading; both delays are evaluated on every call.
- **No policy baked in** — this library only tracks state. Whether a fault
  should sound a buzzer, light an LED, or do anything else (and any
  exceptions, e.g. "this one fault always buzzes regardless of suppression
  rules") is a decision for the layer above, typically driven by
  `onFaultChange()`.
- **Thread-safe** — log methods are queue-backed; fault methods are protected
  by a statically allocated FreeRTOS mutex (cannot fail on OOM) with a bounded
  wait and a safe fallback instead of blocking forever.
- Invalid fault indexes and internal errors are reported via `ESP_LOGE`, never
  silently ignored.

## Installation

### PlatformIO

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/valachbastl/AP_AlarmLog.git
```

Or with specific version:

```ini
lib_deps =
    https://github.com/valachbastl/AP_AlarmLog.git#v1.0.0
```

### ESP-IDF

The library is also a plain ESP-IDF component (`CMakeLists.txt` +
`idf_component.yml`) — copy or link it into your project's `components/`.

## Usage

### Setup

```cpp
#include "AP_AlarmLog.h"

enum Fault : uint8_t {
    FAULT_LINK_LOST = 0,
    FAULT_SENSOR    = 1,
};

static AP_AlarmLog alarmLog;                    // or AP_AlarmLog alarmLog(AP_AlarmLog::Config(64));

void app_init() {
    if (alarmLog.begin() != ESP_OK) {
        // log queue unavailable - faults still work, log entries are dropped
    }

    // 60 s hold time before a lost link is reported (short outages are ignored)
    alarmLog.configureFault(FAULT_LINK_LOST, AP_AlarmLog::FaultConfig(60000));
    // Sensor: instant on, but must read OK for 10 s before the fault clears
    alarmLog.configureFault(FAULT_SENSOR, AP_AlarmLog::FaultConfig(0, 10000));

    alarmLog.onFaultChange([](uint8_t bit, bool active) {
        // Runs in the reporting task, outside the internal lock -
        // free to call log()/ack() here. Keep it short.
        alarmLog.log(active ? AP_AlarmLog::LogLevel::ERROR : AP_AlarmLog::LogLevel::SUCCESS,
                     "Fault %u %s", bit, active ? "active" : "cleared");
    });
}
```

### Reporting faults

Call periodically (e.g. once per sensor-read cycle) with the current raw
reading — not just on change. Each fault bit should be reported by one task.

```cpp
void sensor_task_loop() {
    alarmLog.reportFault(FAULT_LINK_LOST, !linkConnected());
    alarmLog.reportFault(FAULT_SENSOR, !sensorReadOk());
}
```

### Reacting (policy lives in your project)

```cpp
// Buzz only for active + unacked faults; the link fault always buzzes,
// the others respect a project-specific suppression rule.
void update_buzzer() {
    uint32_t urgent = alarmLog.activeMask() & alarmLog.unackedMask();
    bool buzz = (urgent & (1u << FAULT_LINK_LOST)) || (urgent && !suppressed());
    set_buzzer(buzz);
}

// "Acknowledge" button
void on_ack_button() {
    alarmLog.ackAll();
}
```

### Draining the log (UI task)

```cpp
void drain_log() {
    AP_AlarmLog::LogEntry entry;
    while (alarmLog.popLogEntry(entry)) {
        add_to_log_list(entry.level, entry.uptime, entry.msg);
    }
}
```

## API Reference

### Constructor / setup

| Method | Description |
|--------|-------------|
| `AP_AlarmLog(config)` | Stores config only (`Config(logCapacity)`, default 32 entries) |
| `begin()` | Allocates the log queue. Idempotent. Returns `ESP_OK`, `ESP_ERR_INVALID_ARG` (capacity 0) or `ESP_ERR_NO_MEM` |

### Log

| Method | Description |
|--------|-------------|
| `log(level, fmt, ...)` | Append a `printf`-style message (truncated to 47 bytes); never blocks, dropped if the queue is full |
| `popLogEntry(out)` | Pop the oldest entry; `false` if empty |
| `clearLog()` | Discard all queued entries |
| `droppedLogCount()` | Entries dropped since boot (queue full / not initialized) |

### Faults

| Method | Description |
|--------|-------------|
| `configureFault(bit, cfg)` | Per-fault config, `FaultConfig(debounceMs, clearDelayMs)` — on-delay / off-delay, default 0 = instant |
| `reportFault(bit, present)` | Report the current raw condition; call periodically, not just on change |
| `ack(bit)` / `ackAll()` | Clear the unacked bit for one fault / all faults |
| `isActive(bit)` / `isUnacked(bit)` | Per-fault state |
| `activeMask()` / `unackedMask()` | Bitmask across all faults (bit N = fault N) |
| `hasActive()` / `hasUnacked()` | Any fault currently active / unacked |
| `onFaultChange(callback)` | `callback(bit, active)` on each active↔inactive transition (after on/off-delay); `nullptr` removes it |

### Types and constants

| Name | Description |
|------|-------------|
| `LogLevel` | `INFO`, `WARN`, `ERROR`, `SUCCESS` |
| `LogEntry` | `msg[MSG_LEN]`, `uptime` (s since boot), `level` — trivially copyable |
| `MAX_FAULTS` | 32 (one `uint32_t` bitmask) |
| `MSG_LEN` | 48 (log message buffer incl. terminator) |
| `MUTEX_WAIT_MS` | 50 — max wait for the internal mutex |

## Thread Safety and Limits

- Call `begin()` before other tasks start using the instance.
- Log methods are safe from any task; `popLogEntry()` is meant for one reader.
- Fault methods are safe from any task. On a mutex timeout (50 ms) the call
  is logged and skipped / returns a safe default (inactive, mask 0).
- Report each fault bit from **one** task — callbacks run outside the lock,
  so two tasks reporting the same bit could deliver its callbacks out of order.
- The `onFaultChange` callback runs in the context of the task calling
  `reportFault()`; keep it short and non-blocking.
- **Not ISR-safe** — do not call from interrupt handlers.
- `MAX_FAULTS = 32`. A project that needs more independently tracked faults
  should consolidate related conditions under fewer bits rather than raise
  the limit.

## Author

Petr Adámek

## License

MIT — see [LICENSE](LICENSE).
