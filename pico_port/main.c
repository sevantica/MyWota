#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "Hardware_Access.h"
#include "USB_Logging.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include "MyWota_Hardware_Adapter.h"
#include "Pico_HAL.h"

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
    // Initialize UART for early debug (USB requires FreeRTOS task)
    // stdio_uart_init(); /* Commented out: May conflict with peripheral pins */
    
    // Initialize TinyUSB stack
    tusb_init();
    
    // Initialize SD CS pin HIGH (inactive) to prevent SPI bus conflicts
    // SD card shares SPI0 with LCD, so keep SD CS high until SD is initialized
    gpio_init(6);  // SD_CS_PIN (GPIO 6 = SPI_0_CS1)
    gpio_set_dir(6, GPIO_OUT);
    gpio_put(6, 1);  // CS inactive (high)
    
    printf("Starting UI_PICO_PORT...\n");
    print_memory_stats();
 
    printf("Starting FreeRTOS scheduler...\n");

    /* Register Hardware Interface before starting System task */
    MyWota_Hardware_Adapter_Init();

    Task_Start_System_Task();

    vTaskStartScheduler();

    // Should never reach here
    printf("ERROR: Scheduler returned!\n");
    while (1)
    {
        HAL_GPIO_Write(SYSTEM_COMM_LED_PIN, 1);
        sleep_ms(50);
        HAL_GPIO_Write(SYSTEM_COMM_LED_PIN, 0);
        sleep_ms(50);
    }
}

// FreeRTOS hook functions
void vApplicationIdleHook( void )
{
    static uint32_t idle_count = 0;
    idle_count++;
}

void vApplicationMallocFailedHook( void )
{

    taskDISABLE_INTERRUPTS();
    for( ;; );
}

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
    // CRITICAL: Disable hardware watchdog so we can see the fault
    // watchdog_disable();
    
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
    
    // CRITICAL: Disable hardware watchdog so we can see the fault
    // watchdog_disable();
    
    // Light up the LED to indicate hard fault
    // Note: Direct GPIO access during HardFault can be unsafe.
    // const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    // gpio_init(LED_PIN);
    // gpio_set_dir(LED_PIN, GPIO_OUT);
    // gpio_put(LED_PIN, 1);
    
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
    
    // Heap monitoring disabled - using pure static allocation
    // printf("Free Heap: %lu bytes\n", xPortGetFreeHeapSize());
    // printf("Min Free Heap: %lu bytes\n", xPortGetMinimumEverFreeHeapSize());
    printf("===========================\n");
    
    // Flash LED to indicate hard fault (SOS pattern)
    // Disabled for stability during hard fault
    while (1) {
        busy_wait_ms(2000);
    }
}

// Memory management fault handler
void MemManage_Handler(void)
{
    printf("Memory Management Fault!\n");
    while (1) {
        sleep_ms(200);
    }
}

// Bus fault handler
void BusFault_Handler(void)
{
    printf("Bus Fault!\n");
    while (1) {
        sleep_ms(100);
    }
}

// Usage fault handler
void UsageFault_Handler(void)
{
    printf("Usage Fault!\n");
    while (1) {
        sleep_ms(50);
    }
}

/* FreeRTOS Static Allocation Callbacks
 * NOTE: These must be defined in an object file compiled directly into the
 * executable (not in a static library), otherwise the linker cannot resolve
 * the references from freertos_kernel. */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer,
                                    StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize )
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer,
                                     StackType_t **ppxTimerTaskStackBuffer,
                                     uint32_t *pulTimerTaskStackSize )
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
