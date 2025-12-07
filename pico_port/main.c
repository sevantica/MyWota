#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "../application/Include/Hardware_Access.h"
#include "USB_Logging.h"
#include "../application/Include/System.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// Hard fault register structure
typedef struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t psr;
} HardFaultStackFrame_t;

// Function prototypes
void vApplicationMallocFailedHook( void );
void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName );
void HardFault_Handler(void);
void HardFault_Handler_C(HardFaultStackFrame_t *stack_frame);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);

// Memory monitoring function
void print_memory_stats(void)
{

}

// Entry point
int main()
{
    // Initialize all hardware peripherals (this will initialize USB stdio)
    Hardware_Init();
    
    printf("Starting UI_PICO_PORT...\n");
    print_memory_stats();
 
    printf("Starting FreeRTOS scheduler...\n");

    Task_Start_System_Task();

    vTaskStartScheduler();

    // Should never reach here
    printf("ERROR: Scheduler returned!\n");
    while (1)
    {
        Hardware_LED_On();
        sleep_ms(50);
        Hardware_LED_Off();
        sleep_ms(50);
    }
}

// FreeRTOS hook functions
void vApplicationMallocFailedHook( void )
{

    taskDISABLE_INTERRUPTS();
    for( ;; );
}

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
    // Try to print the task name if possible
    printf("\n=== STACK OVERFLOW DETECTED ===\n");
    printf("Task Handle: 0x%p\n", pxTask);
    
    if (pcTaskName != NULL) {
        printf("Task Name: %s\n", pcTaskName);
    } else {
        printf("Task Name: (null)\n");
    }
    
    printf("===============================\n");
    
    taskDISABLE_INTERRUPTS();
    for( ;; );
}

// Hard fault handler - called when a hard fault occurs
void HardFault_Handler(void) __attribute__((naked));

void HardFault_Handler(void)
{
    // Simplified assembly for Cortex-M0+
    __asm volatile (
        "mov r0, lr \n"
        "movs r1, #4 \n"
        "tst r0, r1 \n"
        "beq _MSP \n"
        "mrs r0, psp \n"
        "b HardFault_Handler_C \n"
        "_MSP: \n"
        "mrs r0, msp \n"
        "b HardFault_Handler_C \n"
    );
}

void HardFault_Handler_C(HardFaultStackFrame_t *stack_frame)
{
    // Disable interrupts to prevent further issues
    taskDISABLE_INTERRUPTS();
    
    // Light up the LED to indicate hard fault
    const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);
    
    // Print debug information if stdio is available
    printf("\n=== HARD FAULT DETECTED ===\n");
    printf("PC: 0x%08lX\n", stack_frame->pc);
    printf("LR: 0x%08lX\n", stack_frame->lr);
    printf("PSR: 0x%08lX\n", stack_frame->psr);
    printf("R0: 0x%08lX\n", stack_frame->r0);
    printf("R1: 0x%08lX\n", stack_frame->r1);
    printf("R2: 0x%08lX\n", stack_frame->r2);
    printf("R3: 0x%08lX\n", stack_frame->r3);
    printf("R12: 0x%08lX\n", stack_frame->r12);
    
    // Get current task information if available
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    if (current_task != NULL) {
        printf("Current Task: %s\n", pcTaskGetName(current_task));
        printf("Stack High Water Mark: %lu words\n", uxTaskGetStackHighWaterMark(current_task));
    }
    
    printf("Free Heap: %lu bytes\n", xPortGetFreeHeapSize());
    printf("Min Free Heap: %lu bytes\n", xPortGetMinimumEverFreeHeapSize());
    printf("===========================\n");
    
    // Flash LED to indicate hard fault (SOS pattern)
    while (1) {
        // SOS pattern: ... --- ...
        for (int i = 0; i < 3; i++) {
            gpio_put(LED_PIN, 1);
            sleep_ms(200);
            gpio_put(LED_PIN, 0);
            sleep_ms(200);
        }
        sleep_ms(500);
        for (int i = 0; i < 3; i++) {
            gpio_put(LED_PIN, 1);
            sleep_ms(600);
            gpio_put(LED_PIN, 0);
            sleep_ms(200);
        }
        sleep_ms(500);
        for (int i = 0; i < 3; i++) {
            gpio_put(LED_PIN, 1);
            sleep_ms(200);
            gpio_put(LED_PIN, 0);
            sleep_ms(200);
        }
        sleep_ms(2000);
    }
}

// Memory management fault handler
void MemManage_Handler(void)
{
    printf("Memory Management Fault!\n");
    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(100);
    }
}

// Bus fault handler
void BusFault_Handler(void)
{
    printf("Bus Fault!\n");
    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(50);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(50);
    }
}

// Usage fault handler
void UsageFault_Handler(void)
{
    printf("Usage Fault!\n");
    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(25);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(25);
    }
}
