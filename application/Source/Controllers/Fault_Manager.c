/**
 * @file Fault_Manager.c
 * @brief Per-unit fault state machine implementation.
 */
#include "Fault_Manager.h"
#include "USB_Logging.h"

#define FAULT_ESCALATE_THRESHOLD     3   /* incidents (any reason) -> FAULT */
#define FAULT_RECOVERY_THRESHOLD     3   /* consecutive successes from DEGRADED -> OK */

#define LOG_CRITICAL_FAULT_EN  1
#define LOG_DEBUG_FAULT_EN     1
#if LOG_CRITICAL_FAULT_EN
    #define LOG_CRITICAL_FAULT(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_FAULT(...)
#endif
#if LOG_DEBUG_FAULT_EN
    #define LOG_DEBUG_FAULT(...)    USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_FAULT(...)
#endif

static RS485_Fault_State_t  s_state         = RS485_FAULT_STATE_OK;
static RS485_Fault_Reason_t s_reason        = RS485_FAULT_REASON_NONE;
static uint32_t             s_incidents     = 0;
static uint8_t              s_recent_inc    = 0; /* recent (window) */
static uint8_t              s_consec_ok     = 0;

void Fault_Manager_Init(void)
{
    s_state      = RS485_FAULT_STATE_OK;
    s_reason     = RS485_FAULT_REASON_NONE;
    s_incidents  = 0;
    s_recent_inc = 0;
    s_consec_ok  = 0;
    LOG_CRITICAL_FAULT("[FAULT] Init - state OK\r\n");
}

void Fault_Manager_Report(RS485_Fault_Reason_t reason)
{
    s_incidents++;
    s_consec_ok = 0;
    s_reason    = reason;

    if (s_state == RS485_FAULT_STATE_FAULT) {
        /* Already latched - just record reason */
        LOG_CRITICAL_FAULT("[FAULT] (already FAULT) +incident reason=%s\r\n",
                           Fault_Manager_GetReasonString(reason));
        return;
    }

    s_recent_inc++;
    if (s_recent_inc >= FAULT_ESCALATE_THRESHOLD) {
        s_state = RS485_FAULT_STATE_FAULT;
        LOG_CRITICAL_FAULT("[FAULT] Escalated to FAULT (reason=%s, incidents=%lu)\r\n",
                           Fault_Manager_GetReasonString(reason),
                           (unsigned long)s_incidents);
    } else {
        s_state = RS485_FAULT_STATE_DEGRADED;
        LOG_CRITICAL_FAULT("[FAULT] DEGRADED reason=%s (%u/%u)\r\n",
                           Fault_Manager_GetReasonString(reason),
                           (unsigned)s_recent_inc, FAULT_ESCALATE_THRESHOLD);
    }
}

void Fault_Manager_NoteSuccess(void)
{
    if (s_state == RS485_FAULT_STATE_DEGRADED) {
        s_consec_ok++;
        if (s_consec_ok >= FAULT_RECOVERY_THRESHOLD) {
            s_state      = RS485_FAULT_STATE_OK;
            s_reason     = RS485_FAULT_REASON_NONE;
            s_recent_inc = 0;
            LOG_CRITICAL_FAULT("[FAULT] Recovered to OK after %u successes\r\n",
                               (unsigned)s_consec_ok);
        }
    } else if (s_state == RS485_FAULT_STATE_OK) {
        if (s_recent_inc > 0) s_recent_inc--;
    }
    /* FAULT requires explicit clear */
}

void Fault_Manager_Clear(void)
{
    LOG_CRITICAL_FAULT("[FAULT] Cleared by operator (was state=%d reason=%s)\r\n",
                       (int)s_state, Fault_Manager_GetReasonString(s_reason));
    s_state      = RS485_FAULT_STATE_OK;
    s_reason     = RS485_FAULT_REASON_NONE;
    s_recent_inc = 0;
    s_consec_ok  = 0;
}

RS485_Fault_State_t  Fault_Manager_GetState(void)         { return s_state; }
RS485_Fault_Reason_t Fault_Manager_GetReason(void)        { return s_reason; }
uint32_t             Fault_Manager_GetIncidentCount(void) { return s_incidents; }

const char* Fault_Manager_GetStateString(RS485_Fault_State_t s)
{
    switch (s) {
        case RS485_FAULT_STATE_OK:       return "OK";
        case RS485_FAULT_STATE_DEGRADED: return "DEGRADED";
        case RS485_FAULT_STATE_FAULT:    return "FAULT";
        default:                         return "?";
    }
}

const char* Fault_Manager_GetReasonString(RS485_Fault_Reason_t r)
{
    switch (r) {
        case RS485_FAULT_REASON_NONE:            return "none";
        case RS485_FAULT_REASON_NO_FLOW:         return "no_flow";
        case RS485_FAULT_REASON_VALVE:           return "valve";
        case RS485_FAULT_REASON_FLOW_SENSOR:     return "flow_sensor";
        case RS485_FAULT_REASON_CARD_AUTH_FAIL:  return "card_auth_fail";
        case RS485_FAULT_REASON_TAMPER:          return "tamper";
        case RS485_FAULT_REASON_WATCHDOG_REBOOT: return "wdt_reboot";
        case RS485_FAULT_REASON_FILTER_EXPIRED:  return "filter_expired";
        case RS485_FAULT_REASON_OTHER:           return "other";
        default:                                 return "?";
    }
}
