#include "System_Command.h"

#include "Dispenser_Controller.h"
#include "Fault_Manager.h"
#include "RTC_Manager.h"
#include "CLI_Processor.h"
#include "USB_Logging.h"

#include <time.h>

static System_Command_Status_t map_dispenser_result(DispenserResult_t result)
{
    switch (result) {
        case DISPENSER_RESULT_OK:
            return SYSTEM_CMD_STATUS_OK;
        case DISPENSER_RESULT_BUSY:
            return SYSTEM_CMD_STATUS_BUSY;
        case DISPENSER_RESULT_NO_CARD:
        case DISPENSER_RESULT_CARD_NOT_READY:
            return SYSTEM_CMD_STATUS_NOT_READY;
        case DISPENSER_RESULT_CARD_ERROR:
        default:
            return SYSTEM_CMD_STATUS_ERROR;
    }
}

static System_Command_Status_t cmd_card_topup(const System_Command_Request_t *request,
                                              System_Command_Response_t *response)
{
    (void)response;
    return (System_Command_Status_t)CLI_RequestCardTopup(NULL, request->param1);
}

static System_Command_Status_t cmd_dispense_start(const System_Command_Request_t *request,
                                                  System_Command_Response_t *response)
{
    (void)response;
    return map_dispenser_result(MIFARE_Dispenser_ManualStart(request->param1));
}

static System_Command_Status_t cmd_dispense_stop(const System_Command_Request_t *request,
                                                 System_Command_Response_t *response)
{
    (void)request;
    (void)response;
    return map_dispenser_result(MIFARE_Dispenser_ManualStop());
}

static System_Command_Status_t cmd_clean_start(const System_Command_Request_t *request,
                                               System_Command_Response_t *response)
{
    (void)response;
    return map_dispenser_result(Dispenser_StartSelfClean(request->param1, request->param2));
}

static System_Command_Status_t cmd_clean_stop(const System_Command_Request_t *request,
                                              System_Command_Response_t *response)
{
    (void)request;
    (void)response;
    Dispenser_StopSelfClean();
    return SYSTEM_CMD_STATUS_OK;
}

static System_Command_Status_t cmd_fault_clear(const System_Command_Request_t *request,
                                               System_Command_Response_t *response)
{
    (void)request;
    (void)response;
    Dispenser_ClearLastError();
    Fault_Manager_Clear();
    return SYSTEM_CMD_STATUS_OK;
}

static const System_Command_Definition_t s_mywota_commands[] = {
    {SYSTEM_CMD_ID_CARD_TOPUP,      "card.topup",      "card topup",             SYSTEM_CMD_AUTH_ADMIN, cmd_card_topup},
    {SYSTEM_CMD_ID_DISPENSE_START,  "dispense.start",  "manual dispense start",  SYSTEM_CMD_AUTH_ADMIN, cmd_dispense_start},
    {SYSTEM_CMD_ID_DISPENSE_STOP,   "dispense.stop",   "manual dispense stop",   SYSTEM_CMD_AUTH_ADMIN, cmd_dispense_stop},
    {SYSTEM_CMD_ID_CLEAN_START,     "clean.start",     "self-clean start",       SYSTEM_CMD_AUTH_ADMIN, cmd_clean_start},
    {SYSTEM_CMD_ID_CLEAN_STOP,      "clean.stop",      "self-clean stop",        SYSTEM_CMD_AUTH_ADMIN, cmd_clean_stop},
    {SYSTEM_CMD_ID_FAULT_CLEAR,     "fault.clear",     "fault clear",            SYSTEM_CMD_AUTH_ADMIN, cmd_fault_clear},
};

const System_Command_Definition_t *System_Command_ProjectGetCommands(void)
{
    return s_mywota_commands;
}

size_t System_Command_ProjectGetCommandCount(void)
{
    return sizeof(s_mywota_commands) / sizeof(s_mywota_commands[0]);
}
