/**
 * @file System_Events.h
 * @brief Legacy event system definitions for display and logic
 */

#ifndef SYSTEM_EVENTS_H
#define SYSTEM_EVENTS_H

#include <stdint.h>

/* Event message types */
typedef enum {
    EVENT_TYPE_RFID_PICC = 0,
    EVENT_TYPE_GPIO_PIN = 1,
    EVENT_TYPE_SENSOR = 2,
    EVENT_TYPE_SYSTEM_STATE = 4,
    EVENT_TYPE_USER_INPUT = 5,
    EVENT_TYPE_FLOW_SENSOR = 6,
    EVENT_TYPE_I2C_DEVICE = 7,
    EVENT_TYPE_CUSTOM = 255
} EVENT_TYPE_Enum;

/* Event source identifiers */
typedef enum {
    EVENT_SOURCE_RFID_RC522 = 0,
    EVENT_SOURCE_CAT9555_PIN = 1,
    EVENT_SOURCE_PICO_GPIO = 2,
    EVENT_SOURCE_YS_S201_FLOW = 3,
    EVENT_SOURCE_UI_BUTTON = 4,
    EVENT_SOURCE_SYSTEM_TASK = 5,
    EVENT_SOURCE_LCD_TASK = 6,
    EVENT_SOURCE_CUSTOM = 255
} EVENT_SOURCE_Enum;

/* Event data structure */
typedef union {
    struct {
        uint8_t picc_position;
        uint8_t picc_state;
        uint32_t card_uid;
    } rfid_data;
    
    struct {
        uint8_t pin_id;
        uint8_t pin_state;
        uint8_t bank_id;
        uint32_t debounce_time_ms;
    } gpio_data;
    
    struct {
        uint8_t sensor_id;
        uint8_t sensor_type;
        uint32_t sensor_value;
        uint8_t sensor_status;
    } sensor_data;
} EVENT_DATA_Union;

/* Event message structure */
typedef struct {
    uint64_t message_id;
    EVENT_TYPE_Enum event_type;
    EVENT_SOURCE_Enum event_source;
    uint8_t priority;
    EVENT_DATA_Union event_data;
    uint64_t timestamp;
} DISPLAY_MSG_Def;

typedef DISPLAY_MSG_Def EVENT_MSG_Def;

#endif /* SYSTEM_EVENTS_H */
