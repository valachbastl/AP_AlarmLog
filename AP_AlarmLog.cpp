#include "AP_AlarmLog.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdarg>
#include <cstdio>
#include <utility>

const char *AP_AlarmLog::TAG = "AP_AlarmLog";

AP_AlarmLog::AP_AlarmLog(const Config &config)
    : _logCapacity(config.logCapacity)
{
    _mutex = xSemaphoreCreateMutexStatic(&_mutexBuffer);
}

AP_AlarmLog::~AP_AlarmLog()
{
    if (_logQueue) vQueueDelete(_logQueue);
    if (_mutex) vSemaphoreDelete(_mutex);
}

esp_err_t AP_AlarmLog::begin()
{
    if (_logQueue) return ESP_OK;  // idempotent
    if (_logCapacity == 0) {
        ESP_LOGE(TAG, "begin: logCapacity must be > 0");
        return ESP_ERR_INVALID_ARG;
    }
    _logQueue = xQueueCreate(_logCapacity, sizeof(LogEntry));
    if (!_logQueue) {
        ESP_LOGE(TAG, "begin: log queue allocation failed (%u entries)", (unsigned)_logCapacity);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ── Internal helpers ─────────────────────────────────────────────────────────

bool AP_AlarmLog::_lock(const char *caller) const
{
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) == pdTRUE) return true;
    ESP_LOGE(TAG, "%s: mutex timeout", caller);
    return false;
}

void AP_AlarmLog::_unlock() const
{
    xSemaphoreGive(_mutex);
}

bool AP_AlarmLog::_validBit(uint8_t bit, const char *caller) const
{
    if (bit < MAX_FAULTS) return true;
    ESP_LOGE(TAG, "%s: invalid fault bit %u (max %u)", caller, bit, MAX_FAULTS - 1);
    return false;
}

// ── Log ──────────────────────────────────────────────────────────────────────
// Queue-backed (a FreeRTOS queue is already safe for N writers / 1 reader) -
// the fault mutex is not used here.

void AP_AlarmLog::log(LogLevel level, const char *fmt, ...)
{
    if (!_logQueue) {
        _droppedLogs.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    LogEntry entry = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(entry.msg, sizeof(entry.msg), fmt, args);
    va_end(args);
    entry.uptime = (uint32_t)(esp_timer_get_time() / 1000000);
    entry.level  = level;

    // Full queue -> drop and count, never block the calling task
    if (xQueueSend(_logQueue, &entry, 0) != pdTRUE) {
        _droppedLogs.fetch_add(1, std::memory_order_relaxed);
    }
}

bool AP_AlarmLog::popLogEntry(LogEntry &out)
{
    if (!_logQueue) return false;
    return xQueueReceive(_logQueue, &out, 0) == pdTRUE;
}

void AP_AlarmLog::clearLog()
{
    if (_logQueue) xQueueReset(_logQueue);
}

uint32_t AP_AlarmLog::droppedLogCount() const
{
    return _droppedLogs.load(std::memory_order_relaxed);
}

// ── Faults ───────────────────────────────────────────────────────────────────

void AP_AlarmLog::configureFault(uint8_t bit, const FaultConfig &cfg)
{
    if (!_validBit(bit, "configureFault") || !_lock("configureFault")) return;
    _faults[bit].config = cfg;
    _unlock();
}

void AP_AlarmLog::reportFault(uint8_t bit, bool present)
{
    if (!_validBit(bit, "reportFault") || !_lock("reportFault")) return;

    FaultState &f = _faults[bit];
    const uint32_t bitMask   = (1u << bit);
    const int64_t  nowUs     = esp_timer_get_time();
    const bool     wasActive = (_activeMask & bitMask) != 0;

    bool becameActive  = false;
    bool becameCleared = false;

    if (present) {
        if (!f.rawPresent) {
            f.rawPresent     = true;
            f.presentSinceUs = nowUs;
        }
        if (!wasActive) {
            const int64_t heldUs = nowUs - f.presentSinceUs;
            if (heldUs >= (int64_t)f.config.debounceMs * 1000) {
                _activeMask  |= bitMask;
                _unackedMask |= bitMask;  // fresh inactive->active transition only
                becameActive = true;
            }
        }
    } else {
        if (f.rawPresent) {
            f.rawPresent    = false;
            f.absentSinceUs = nowUs;
        }
        if (wasActive) {
            // Off-delay: a condition that returns before it expires keeps the
            // fault ACTIVE (no new cycle, no new ack needed)
            const int64_t absentUs = nowUs - f.absentSinceUs;
            if (absentUs >= (int64_t)f.config.clearDelayMs * 1000) {
                _activeMask &= ~bitMask;  // unacked bit intentionally left as-is
                becameCleared = true;
            }
        }
    }

    // Copy the callback while still holding the lock - reading the
    // std::function concurrently with onFaultChange() reassigning it would
    // otherwise be a data race.
    std::function<void(uint8_t, bool)> cb;
    if (becameActive || becameCleared) cb = _onFaultChangeCb;

    _unlock();

    // Invoked outside the lock, against the local copy - may safely call
    // back into this instance (e.g. log(), ack()).
    if (cb) cb(bit, becameActive);
}

void AP_AlarmLog::ack(uint8_t bit)
{
    if (!_validBit(bit, "ack") || !_lock("ack")) return;
    _unackedMask &= ~(1u << bit);
    _unlock();
}

void AP_AlarmLog::ackAll()
{
    if (!_lock("ackAll")) return;
    _unackedMask = 0;
    _unlock();
}

bool AP_AlarmLog::isActive(uint8_t bit) const
{
    // Safe default on error/timeout: can't confirm -> don't claim active
    if (!_validBit(bit, "isActive") || !_lock("isActive")) return false;
    bool result = (_activeMask & (1u << bit)) != 0;
    _unlock();
    return result;
}

bool AP_AlarmLog::isUnacked(uint8_t bit) const
{
    if (!_validBit(bit, "isUnacked") || !_lock("isUnacked")) return false;
    bool result = (_unackedMask & (1u << bit)) != 0;
    _unlock();
    return result;
}

uint32_t AP_AlarmLog::activeMask() const
{
    if (!_lock("activeMask")) return 0;
    uint32_t result = _activeMask;
    _unlock();
    return result;
}

uint32_t AP_AlarmLog::unackedMask() const
{
    if (!_lock("unackedMask")) return 0;
    uint32_t result = _unackedMask;
    _unlock();
    return result;
}

bool AP_AlarmLog::hasActive() const
{
    return activeMask() != 0;
}

bool AP_AlarmLog::hasUnacked() const
{
    return unackedMask() != 0;
}

void AP_AlarmLog::onFaultChange(std::function<void(uint8_t bit, bool active)> callback)
{
    if (!_lock("onFaultChange")) return;
    _onFaultChangeCb = std::move(callback);
    _unlock();
}
