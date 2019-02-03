/*
 * debug_helper.c
 * Tools for debugging ExplorerLink.
 *
 * Copyright 2018, 2019 Matt Rounds
 *
 * This file is part of ExplorerLink.
 *
 * ExplorerLink is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * ExplorerLink is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ExplorerLink. If not, see <https://www.gnu.org/licenses/>.
 */


#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_gpio.h"
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "driverlib/uart.h"
#include "utils/uartstdio.h"
#include "analog_task.h"
#include "can_task.h"
#include "data_task.h"
#include "jsn_task.h"
#include "modem_mgmt_task.h"
#include "modem_uart_task.h"
#include "remote_start_task.h"
#include "srf_task.h"
#include "semphr.h"


/* The mutex that protects concurrent access of UART from multiple tasks. */
xSemaphoreHandle g_pUARTSemaphore;

/* Counter to be incremented at 10kHz for the FreeRTOS kernel's runtime
 * statistics. See the timer setup and ISR below, as well as the associated
 * FreeRTOS macro definitions in FreeRTOSConfig.h. */
volatile uint32_t ulRuntimeStatsCounter = 0;

/* This global stores the last value on the debug status bus (PF0-4). When
 * FreeRTOS calls traceTASK_SWITCHED_IN(), this value is updated with the
 * task's tag number. When FreeRTOS calls traceTASK_SWITCHED_OUT(), it is
 * set back to 0. When an ISR runs, it will put its own tag value on the port F
 * bus upon entry. On exit, this value is restored. In this way, if a task is
 * interrupted, the bus will still indicate the correct tag value once the ISR
 * completes and execution returns to the task.
 *
 * If ISRs nest, the bus value may be incorrect after the highest priority ISR
 * completes and execution returns to the previous ISR. The value will still
 * be restored to the correct task tag value once the lowest priority ISR
 * exits. */
volatile uint32_t ulLastPortFValue = 0;

/*
 * ISR for the runtime stats counter. Configured to interrupt at 10kHz below.
 * Increments ulRuntimeStatsCounter, which serves as a "clock" for the kernel's
 * statistics functionality.
 */
void
WTimer2AIntHandler(void) {
    uint32_t ulStatus;

    GPIOPinWrite( GPIO_PORTF_BASE, UINT32_MAX, 18 );

    // Read the (masked) interrupt status of the timer module.
    ulStatus = TimerIntStatus(WTIMER2_BASE, true);

    // Clear any pending status.
    TimerIntClear(WTIMER2_BASE, ulStatus);

    if (ulStatus & TIMER_TIMA_TIMEOUT) {
        /* This was a timeout interrupt. Increment the runtime stats counter. */
        ulRuntimeStatsCounter++;
    }

    GPIOPinWrite( GPIO_PORTF_BASE, UINT32_MAX, ulLastPortFValue );
}

static void
TestHelperGPIOConfigure(void) {

    /* Enable GPIO port B. */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);

    /* Wait for port B to become ready. */
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB)) {
    }

    /* Set initial output state to low for 5v enable. */
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_3, 0);

    /* 5v enable active-high, so wpd */
    GPIOPadConfigSet(GPIO_PORTB_BASE, GPIO_PIN_3,
                     GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD_WPD);

    /* PB3 is an output. */
    GPIODirModeSet(GPIO_PORTB_BASE, GPIO_PIN_3, GPIO_DIR_MODE_OUT);




    /* Enable GPIO port F. When debugging, port F functions as a "program
     * status bus"; its 5 output pins allow for 32 unique output statuses.
     * These raw GPIO signals can be measured by a logic analyzer/logger to see
     * program execution move through various states (e.g. ISR entry, task
     * entry). */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);

    /* Wait for port F to become ready. */
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF)) {
    }

    /* PF0 is locked by default because it can be used as a NMI input. This
     * sequence unlocks it, allowing it to be configured. */
    HWREG(GPIO_PORTF_BASE+GPIO_O_LOCK) = GPIO_LOCK_KEY;
    HWREG(GPIO_PORTF_BASE+GPIO_O_CR)   |= GPIO_PIN_0;
    HWREG(GPIO_PORTF_BASE+GPIO_O_LOCK) = 0x0;

    /* Set initial output state to low. */
    GPIOPinWrite( GPIO_PORTF_BASE, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4, 0);

    /* 8mA drive strength, weak pulldown */
    GPIOPadConfigSet(GPIO_PORTF_BASE, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4,
                     GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD_WPD);

    /* Set all to outputs. */
    GPIODirModeSet(GPIO_PORTF_BASE, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4, GPIO_DIR_MODE_OUT);

}

/*
 * This function is associated with the portCONFIGURE_TIMER_FOR_RUN_TIME_STATS
 * macro in FreeRTOSConfig.h. The kernel will call it when the program starts.
 * All this function does is configure a timer that will interrupt at 10kHz.
 * See the WTimer2IntHandler ISR above.
 */
void
vSetupTimerForRunTimeStats( void ) {

    /* Enable clocking for Wide Timer 2. */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_WTIMER2);

    /* Wait for Wide Timer 2 to become ready. */
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_WTIMER2)) {
    }

    /* Configure Wide Timer 2 such that its A-half will count down in periodic
     * mode. */
    TimerConfigure(WTIMER2_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PERIODIC);

    /* Set the starting value to 1/10,000 seconds */
    TimerLoadSet(WTIMER2_BASE, TIMER_A, 8000);

    /* This should not be needed, but can't hurt. */
    TimerIntClear(WTIMER2_BASE, TIMER_TIMA_TIMEOUT);

    /* Enable interrupts on timeout. */
    TimerIntEnable(WTIMER2_BASE, TIMER_TIMA_TIMEOUT);

    /* Enable interrupts at the NVIC. */
    IntEnable(INT_WTIMER2A);

    /* Begin counting. */
    TimerEnable(WTIMER2_BASE, TIMER_A);
}

/*
 * Configure the console UART and its pins. This must be called before
 * UARTprintf().
 */
void
ConfigureUART0(void) {

    /* Enable the GPIO Peripheral used by the UART. */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);

    /* Enable UART0. */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);

    /* Configure GPIO Pins for UART mode. */
    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    /* Use the internal 16MHz oscillator as the UART clock source. */
    UARTClockSourceSet(UART0_BASE, UART_CLOCK_PIOSC);

    /* Initialize the UART for console I/O. */
    UARTStdioConfig(0, 115200, 16000000);
}

void
DebugHelperInit(void) {

    /* Initialize UART0 and configure it for 115,200, 8-N-1 operation. */
    ConfigureUART0();

    /* Create a mutex to guard the console UART. */
    g_pUARTSemaphore = xSemaphoreCreateMutex();

    vTaskSetApplicationTaskTag( xAnalogTaskHandle,      ( void * ) 1 );
    vTaskSetApplicationTaskTag( xCANTaskHandle,         ( void * ) 3 );
    vTaskSetApplicationTaskTag( xDataTaskHandle,        ( void * ) 5 );
    vTaskSetApplicationTaskTag( xJSNTaskHandle,         ( void * ) 31 );
    vTaskSetApplicationTaskTag( xModemMgmtTaskHandle,   ( void * ) 10 );
    vTaskSetApplicationTaskTag( xModemUARTTaskHandle,   ( void * ) 12 );
    vTaskSetApplicationTaskTag( xRemoteStartTaskHandle, ( void * ) 14 );
    vTaskSetApplicationTaskTag( xSRFTaskHandle,         ( void * ) 16 );

    TestHelperGPIOConfigure();

}
