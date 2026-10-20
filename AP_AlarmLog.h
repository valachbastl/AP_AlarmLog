#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

/**
 * @brief Application event log + latching fault/alarm tracker for embedded UI projects.
 *
 * Two related but independent halves:
 *
 * - Log: a small bounded queue of human-readable status messages (info/warn/
 *   error/success), meant to be drained by a UI task into a scrollable list.
 *   Any task may call log() concurrently - backed by a FreeRTOS queue (safe
 *   for N writers / 1 reader). A full queue drops the new entry rather than
 *   blocking the caller; dropped entries are counted (droppedLogCount()).
 *
 * - Faults: up to MAX_FAULTS independently tracked boolean conditions (e.g.
 *   "connection lost", "sensor X failed"), each identified by a small bit
 *   index. Two independent axes of state per fault - active/resolved and
 *   acknowledged/unacknowledged - matching a standard industrial alarm &
 *   event log:
 *
 *     OK                 --reportFault(true)-->  ACTIVE, UNACKED
 *     ACTIVE, UNACKED     --ack()-->              ACTIVE, ACKED
 *     ACTIVE, UNACKED     --reportFault(false)--> RESOLVED, UNACKED
 *     ACTIVE, ACKED       --reportFault(false)--> OK   (unacked bit already clear)
 *     RESOLVED, UNACKED   --ack()-->              OK
 *
 *   A fault only re-enters UNACKED on a fresh inactive->active transition -
 *   never on the falling edge. ack() only ever clears the unacked bit, it does
 *   not require the fault to have cleared first. An entry therefore only
 *   disappears once it is both resolved and acknowledged, and a fault that
 *   re-occurs after being acked goes through the full cycle again (re-arm).
 *
 * reportFault() applies two optional per-fault delays (FaultConfig):
 * - debounceMs (on-delay): the condition must be present continuously this
 *   long before the fault becomes ACTIVE - filters short glitches.
 * - clearDelayMs (off-delay): the condition must be absent continuously this
 *   long before an ACTIVE fault clears - filters a "chattering" condition
 *   that keeps dropping out for a moment. Without it every return of such a
 *   condition would be a fresh ACTIVE+UNACKED cycle needing another ack.
 * Neither delay ever acknowledges or silences anything - a fault stays
 * UNACKED until ack() is called. Timing uses the monotonic 64-bit esp_timer
 * (immune to FreeRTOS tick drift under DFS/esp_pm, no 32-bit wrap-around).
 * There is no separate tick() - call reportFault() periodically with the
 * current raw condition (not just on change); both delays are evaluated on
 * every call.
 *
 * This class only tracks state - it does not decide whether a fault should
 * sound a buzzer, light an LED, or block anything, and it does not write log
 * entries on its own. That policy belongs one layer above, in the consuming
 * project, typically driven by onFaultChange(). This class has no notion of
 * what a fault means.
 *
 * Thread safety:
 * - begin() must complete before any other task uses the instance.
 * - log()/popLogEntry()/clearLog()/droppedLogCount() are safe from any task
 *   (FreeRTOS queue + atomic counter). popLogEntry() is meant for one reader.
 * - Fault methods are protected by an internal statically allocated mutex
 *   with a bounded wait (MUTEX_WAIT_MS); on timeout the call is logged and
 *   skipped / returns a safe default (inactive / 0) instead of blocking.
 * - Each fault bit should be reported by ONE task. Callbacks are invoked
 *   outside the mutex, so two tasks reporting the same bit could deliver its
 *   callbacks in the opposite order of the state changes.
 * - onFaultChange's callback runs in the context of the task that called
 *   reportFault(), without the mutex held - it may call other methods on
 *   this instance (e.g. log(), ack()). Keep it short and non-blocking.
 * - Not ISR-safe: do not call any method from an interrupt handler.
 */
class AP_AlarmLog
{
public:
    static constexpr uint8_t  MAX_FAULTS    = 32;   ///< one bit per fault in a uint32_t mask
    static constexpr size_t   MSG_LEN       = 48;   ///< log message buffer incl. terminator (longer text is truncated)
    static constexpr uint32_t MUTEX_WAIT_MS = 50;   ///< max wait for the fault mutex [ms]

    enum class LogLevel : uint8_t { INFO, WARN, ERROR, SUCCESS };

    struct LogEntry {
        char     msg[MSG_LEN];
        uint32_t uptime;  ///< seconds since boot, from esp_timer (wraps after ~136 years)
        LogLevel level;
    };
    // Copied byte-wise through a FreeRTOS queue - must stay a plain data struct
    // (no owning pointers, std::string, user-defined copy ctor or dtor), so that
    // a future change fails to compile here instead of corrupting data at runtime.
    static_assert(std::is_trivially_copyable<LogEntry>::value,
                  "AP_AlarmLog::LogEntry must remain trivially copyable");

    // Defaults live in the constructors, not as default member initializers:
    // a nested struct with member initializers cannot be used as a default
    // argument of an enclosing-class method until the enclosing class is
    // complete ([class.mem]). Construct as Config(64) / FaultConfig(60000, 5000).
    struct Config {
        size_t logCapacity;  ///< max queued log entries before new ones are dropped
        explicit Config(size_t logCapacity_ = 32) : logCapacity(logCapacity_) {}
    };

    struct FaultConfig {
        uint32_t debounceMs;    ///< on-delay: condition present this long before ACTIVE (0 = instant)
        uint32_t clearDelayMs;  ///< off-delay: condition absent this long before an ACTIVE fault clears (0 = instant)
        explicit FaultConfig(uint32_t debounceMs_ = 0, uint32_t clearDelayMs_ = 0)
            : debounceMs(debounceMs_), clearDelayMs(clearDelayMs_) {}
    };

    /** @brief Stores the configuration only - no allocation besides the static mutex. */
    explicit AP_AlarmLog(const Config &config = Config());
    ~AP_AlarmLog();

    AP_AlarmLog(const AP_AlarmLog &) = delete;
    AP_AlarmLog &operator=(const AP_AlarmLog &) = delete;

    /**
     * @brief Allocates the internal log queue. Idempotent. Call once before
     *        other tasks use the instance. Faults work even if this fails.
     * @return ESP_OK on success (or if already initialized),
     *         ESP_ERR_INVALID_ARG if logCapacity is 0,
     *         ESP_ERR_NO_MEM if the queue could not be allocated
     */
    esp_err_t begin();

    // ── Log ──────────────────────────────────────────────────────────────────

    /**
     * @brief Append a printf-style formatted message (truncated to MSG_LEN-1
     *        bytes). Never blocks: if the queue is full - or begin() has not
     *        succeeded - the entry is dropped and counted in droppedLogCount().
     */
    void log(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

    /**
     * @brief Pop the oldest queued entry. Non-blocking.
     * @return true if an entry was returned, false if the queue is empty
     */
    bool popLogEntry(LogEntry &out);

    /** @brief Discard all currently queued entries (does not reset droppedLogCount()). */
    void clearLog();

    /** @brief Number of log entries dropped since boot (queue full or not initialized). */
    uint32_t droppedLogCount() const;

    // ── Faults ───────────────────────────────────────────────────────────────

    /**
     * @brief Configure a fault slot. Optional - an unconfigured fault behaves
     *        as FaultConfig() (no delays). Normally called once at startup.
     * @param bit Fault index, 0..MAX_FAULTS-1 (out of range is logged and ignored)
     */
    void configureFault(uint8_t bit, const FaultConfig &cfg = FaultConfig());

    /**
     * @brief Report the current raw state of a condition. Call periodically
     *        (e.g. once per sensor-read cycle) with the live reading, not
     *        just when it changes - on/off-delays and edge detection happen
     *        internally, timed from the first call that reports a change.
     * @param bit     Fault index, 0..MAX_FAULTS-1 (out of range is logged and ignored)
     * @param present Current raw condition (true = fault condition present)
     */
    void reportFault(uint8_t bit, bool present);

    /** @brief Acknowledge one fault - clears its unacked bit whether or not it is still active. */
    void ack(uint8_t bit);

    /** @brief Acknowledge every currently unacked fault. */
    void ackAll();

    bool isActive(uint8_t bit) const;
    bool isUnacked(uint8_t bit) const;

    /** @brief Bitmask of currently active faults (bit N = fault N). */
    uint32_t activeMask() const;

    /** @brief Bitmask of currently unacked faults (active or resolved-but-unacked). */
    uint32_t unackedMask() const;

    bool hasActive() const;
    bool hasUnacked() const;

    /**
     * @brief Registers a callback fired whenever a fault transitions between
     *        active and inactive (after on/off-delay). Replaces any previous one;
     *        pass nullptr to remove it. See "Thread safety" above.
     * @param callback Invoked as callback(bit, active) - active=true on the
     *                 ACTIVE transition, false when it clears.
     */
    void onFaultChange(std::function<void(uint8_t bit, bool active)> callback);

private:
    struct FaultState {
        FaultConfig config;
        bool    rawPresent     = false;  // last raw reading passed to reportFault()
        int64_t presentSinceUs = 0;      // esp_timer_get_time() when rawPresent last went true
        int64_t absentSinceUs  = 0;      // esp_timer_get_time() when rawPresent last went false
    };

    bool _lock(const char *caller) const;
    void _unlock() const;
    bool _validBit(uint8_t bit, const char *caller) const;

    FaultState _faults[MAX_FAULTS];
    uint32_t   _activeMask  = 0;
    uint32_t   _unackedMask = 0;

    std::function<void(uint8_t, bool)> _onFaultChangeCb;

    QueueHandle_t         _logQueue = nullptr;
    size_t                _logCapacity;
    std::atomic<uint32_t> _droppedLogs{0};

    mutable SemaphoreHandle_t _mutex;
    StaticSemaphore_t         _mutexBuffer;  // static storage - mutex creation cannot fail on OOM

    static const char *TAG;
};
