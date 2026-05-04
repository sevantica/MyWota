/**
 * @file Fault_Manager.h
 * @brief Per-unit fault state machine (slave-side).
 *
 * Tracks transient anomalies that happen during dispense and escalates them
 * into a latched FAULT state so the master / operator can react. State is
 * exposed via RS485 device status (fault_state, fault_reason).
 *
 * Transitions:
 *   OK         -- 1 incident      --> DEGRADED
 *   DEGRADED   -- N incidents     --> FAULT (latched)
 *   DEGRADED   -- M consecutive ok dispenses --> OK
 *   FAULT      -- explicit clear  --> OK
 */
#ifndef FAULT_MANAGER_H
#define FAULT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "RS485_Protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void Fault_Manager_Init(void);

/** Report an anomaly. Increments incident count, may escalate state. */
void Fault_Manager_Report(RS485_Fault_Reason_t reason);

/** Note a successful dispense. Recovers from DEGRADED after enough successes. */
void Fault_Manager_NoteSuccess(void);

/** Operator-driven clear (USB / RS485). Wipes latched FAULT back to OK. */
void Fault_Manager_Clear(void);

RS485_Fault_State_t  Fault_Manager_GetState(void);
RS485_Fault_Reason_t Fault_Manager_GetReason(void);

/** @return Total incident count since boot (for diagnostics). */
uint32_t Fault_Manager_GetIncidentCount(void);

const char* Fault_Manager_GetStateString(RS485_Fault_State_t s);
const char* Fault_Manager_GetReasonString(RS485_Fault_Reason_t r);

#ifdef __cplusplus
}
#endif
#endif /* FAULT_MANAGER_H */
