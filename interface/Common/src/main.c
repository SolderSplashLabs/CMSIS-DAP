/* CMSIS-DAP Interface Firmware
 * Copyright (c) 2009-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <RTL.h>
#include "rl_usb.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "main.h"
#include "gpio.h"
#include "uart.h"
#include "semihost.h"
#include "tasks.h"

#include "target_reset.h"
#include "swd_host.h"
#include "version.h"

// Event flags for main task
// Timers events
#define FLAGS_MAIN_90MS           (1 << 0)
#define FLAGS_MAIN_30MS           (1 << 1)

// Reset events
#define FLAGS_MAIN_RESET          (1 << 2)

// USB Events
#define FLAGS_MAIN_USB_DISCONNECT (1 << 3)

// Other Events
#define FLAGS_MAIN_POWERDOWN      (1 << 4)
#define FLAGS_MAIN_DISABLEDEBUG   (1 << 5)

// Used by msd when flashing a new binary
#define FLAGS_LED_BLINK_30MS      (1 << 6)

// Timing constants (in 90mS ticks)
// USB busy time
#define USB_BUSY_TIME           (10)
// Delay before a USB device connect may occur
#define USB_CONNECT_DELAY       (2)
// Delay before target may be taken out of reset or reprogrammed after startup
#define STARTUP_DELAY           (0)
// Decrement to zero
#define DECZERO(x)              (x ? --x : 0)

// LED state
typedef enum {
    LED_OFF,
    LED_FLASH,
    LED_FLASH_PERMANENT
} LED_STATE;

// Reference to our main task
OS_TID main_task_id;
OS_TID serial_task_id;

// USB busy LED state; when TRUE the LED will flash once using 30mS clock tick
static uint8_t dap_led_usb_activity = 0;
static uint8_t cdc_led_usb_activity = 0;
static uint8_t msd_led_usb_activity = 0;

static LED_STATE dap_led_state = LED_FLASH;
static LED_STATE cdc_led_state = LED_FLASH;
static LED_STATE msd_led_state = LED_FLASH;

static uint8_t send_uID = 0;

// Global state of usb
USB_CONNECT usb_state;

static USB_BUSY usb_busy;
static uint32_t usb_busy_count;

static U64 stk_timer_30_task[TIMER_TASK_30_STACK/8];
static U64 stk_dap_task[DAP_TASK_STACK/8];
static U64 stk_serial_task[SERIAL_TASK_STACK/8];
static U64 stk_main_task[MAIN_TASK_STACK/8];

// Timer task, set flags every 30mS and 90mS
__task void timer_task_30mS(void) {
    uint8_t i = 0;
    os_itv_set(3); // 30mS

    while(1) {
        os_itv_wait();
        os_evt_set(FLAGS_MAIN_30MS, main_task_id);
        if (!(i++ % 3))
            os_evt_set(FLAGS_MAIN_90MS, main_task_id);
    }
}

// Functions called from other tasks to trigger events in the main task
void main_reset_target(uint8_t send_unique_id) {
    if (send_unique_id) {
        send_uID = 1;
    }
    os_evt_set(FLAGS_MAIN_RESET, main_task_id);
    return;
}

// Flash DAP LED using 30mS tick
void main_blink_dap_led(uint8_t permanent) {
    dap_led_usb_activity=1;
    dap_led_state = (permanent) ? LED_FLASH_PERMANENT : LED_FLASH;
    return;
}

// Flash Serial LED using 30mS tick
void main_blink_cdc_led(uint8_t permanent) {
    cdc_led_usb_activity=1;
    cdc_led_state = (permanent) ? LED_FLASH_PERMANENT : LED_FLASH;
    return;
}

// Flash MSD LED using 30mS tick
void main_blink_msd_led(uint8_t permanent) {
    msd_led_usb_activity=1;
    msd_led_state = (permanent) ? LED_FLASH_PERMANENT : LED_FLASH;
    return;
}

// MSC data transfer in progress
void main_usb_busy_event(void) {
    usb_busy_count = USB_BUSY_TIME;
    usb_busy = USB_ACTIVE;
    //os_evt_set(FLAGS_MAIN_USB_BUSY, main_task_id);
    return;
}

// A new binary has been flashed in the target
void main_usb_disconnect_event(void) {
    os_evt_set(FLAGS_MAIN_USB_DISCONNECT, main_task_id);
    return;
}

// Power down the interface
void main_powerdown_event(void) {
    os_evt_set(FLAGS_MAIN_POWERDOWN, main_task_id);
    return;
}

// Disable debug on target
void main_disable_debug_event(void) {
    os_evt_set(FLAGS_MAIN_DISABLEDEBUG, main_task_id);
    return;
}

#define SIZE_DATA (64)

__task void serial_process() {
    uint8_t data[SIZE_DATA];
    uint8_t len_data = 0;

    while (1) {

        len_data = uart_read_data(data, SIZE_DATA);
        if (len_data) {
            if(USBD_CDC_ACM_DataSend(data , len_data))
                main_blink_cdc_led(0);
        }

        len_data = USBD_CDC_ACM_DataRead(data, SIZE_DATA);
        if (len_data) {
            if (uart_write_data(data, len_data))
                main_blink_cdc_led(0);
        }
    }
}

extern __task void hid_process(void);
__task void main_task(void) {
    // State processing
    uint16_t flags;

    // LED
    uint8_t dap_led_value = 1;
    uint8_t cdc_led_value = 1;
    uint8_t msd_led_value = 1;

    // USB
    uint32_t usb_state_count;

    // thread running after usb connected started
    uint8_t thread_started = 0;

    // button state
    char button_activated;

    // string containing unique ID
    uint8_t * id_str;

    // Get a reference to this task
    main_task_id = os_tsk_self();

    // leds
    gpio_init();

    usbd_init();
    swd_init();

    // Turn on LED
    gpio_set_dap_led(1);
    gpio_set_cdc_led(1);
    gpio_set_msd_led(1);

    // Setup reset button
    gpio_enable_button_flag(main_task_id, FLAGS_MAIN_RESET);
    button_activated = 1;

    // USB
    usbd_connect(0);
    usb_busy = USB_IDLE;
    usb_busy_count = 0;
    usb_state = USB_CONNECTING;
    usb_state_count = USB_CONNECT_DELAY;

    // Update HTML version information file
    update_html_file();

    // Start timer tasks
    os_tsk_create_user(timer_task_30mS, TIMER_TASK_30_PRIORITY, (void *)stk_timer_30_task, TIMER_TASK_30_STACK);

    // Target running
    target_set_state(RESET_RUN_WITH_DEBUG);

    // start semihost task
    semihost_init();
    semihost_enable();

    while(1) {
        os_evt_wait_or(   FLAGS_MAIN_RESET              // Put target in reset state
                        | FLAGS_MAIN_90MS               // 90mS tick
                        | FLAGS_MAIN_30MS               // 30mS tick
                        | FLAGS_MAIN_POWERDOWN          // Power down interface
                        | FLAGS_MAIN_DISABLEDEBUG       // Power down interface
                        | FLAGS_MAIN_USB_DISCONNECT,    // Disable target debug
                        NO_TIMEOUT);

        // Find out what event happened
        flags = os_evt_get();

        if (flags & FLAGS_MAIN_USB_DISCONNECT) {
            usb_busy = USB_IDLE;                         // USB not busy
            usb_state_count = 4;
            usb_state = USB_DISCONNECT_CONNECT;        // disconnect the usb
        }

        if (flags & FLAGS_MAIN_RESET) {
            cdc_led_state = LED_OFF;
            gpio_set_cdc_led(0);
            //usbd_cdc_ser_flush();
            if (send_uID) {
                // set the target in reset to not receive char on the serial port
                target_set_state(RESET_HOLD);

                // send uid
                id_str = get_uid_string();
                USBD_CDC_ACM_DataSend(id_str, strlen((const char *)id_str));
                send_uID = 0;
            }
            // Reset target
            target_set_state(RESET_RUN);
            cdc_led_state = LED_FLASH;
            gpio_set_cdc_led(1);
            button_activated = 0;
        }

        if (flags & FLAGS_MAIN_POWERDOWN) {
            // Stop semihost task
            semihost_disable();

            // Disable debug
            target_set_state(NO_DEBUG);

            // Disconnect USB
            usbd_connect(0);

            // Turn off LED
            gpio_set_dap_led(0);
            gpio_set_cdc_led(0);
            gpio_set_msd_led(0);

            // TODO: put the interface chip in sleep mode
            while (1) {    }
        }

        if (flags & FLAGS_MAIN_DISABLEDEBUG) {
            // Stop semihost task
            semihost_disable();

            // Disable debug
            target_set_state(NO_DEBUG);
        }

        if (flags & FLAGS_MAIN_90MS) {
            if (!button_activated) {
                gpio_enable_button_flag(main_task_id, FLAGS_MAIN_RESET);
                button_activated = 1;
            }

            // Update USB busy status
            switch (usb_busy) {

                case USB_ACTIVE:
                    if (DECZERO(usb_busy_count) == 0) {
                        usb_busy=USB_IDLE;
                    }
                    break;

                case USB_IDLE:
                default:
                    break;
            }

            // Update USB connect status
            switch (usb_state) {

                case USB_DISCONNECTING:
                    // Wait until USB is idle before disconnecting
                    if (usb_busy == USB_IDLE) {
                        usbd_connect(0);
                        usb_state = USB_DISCONNECTED;
                    }
                    break;

                case USB_DISCONNECT_CONNECT:
                    // Wait until USB is idle before disconnecting
                    if ((usb_busy == USB_IDLE) && (DECZERO(usb_state_count) == 0)) {
                        usbd_connect(0);
                        usb_state = USB_CONNECTING;

                        // Update HTML file
                        update_html_file();
                    }
                    break;

                case USB_CONNECTING:
                    // Wait before connecting
                    if (DECZERO(usb_state_count) == 0) {
                        usbd_connect(1);
                        usb_state = USB_CHECK_CONNECTED;
                    }
                    break;

                case USB_CHECK_CONNECTED:
                    if(usbd_configured()) {
                        if (!thread_started) {
                            os_tsk_create_user(hid_process, DAP_TASK_PRIORITY, (void *)stk_dap_task, DAP_TASK_STACK);
                            serial_task_id = os_tsk_create_user(serial_process, SERIAL_TASK_PRIORITY, (void *)stk_serial_task, SERIAL_TASK_STACK);
                            thread_started = 1;
                        }
                        usb_state = USB_CONNECTED;
                    }
                    break;

                case USB_CONNECTED:
                case USB_DISCONNECTED:
                default:
                    break;
            }
         }

        // 30mS tick used for flashing LED when USB is busy
        if (flags & FLAGS_MAIN_30MS) {
            if (dap_led_usb_activity && ((dap_led_state == LED_FLASH) || (dap_led_state == LED_FLASH_PERMANENT))) {
                // Flash DAP LED ONCE
                if (dap_led_value) {
                    dap_led_value = 0;
                } else {
                    dap_led_value = 1; // Turn on
                    if (dap_led_state == LED_FLASH) {
                        dap_led_usb_activity = 0;
                    }
                }

                // Update hardware
                gpio_set_dap_led(dap_led_value);
            }

            if (msd_led_usb_activity && ((msd_led_state == LED_FLASH) || (msd_led_state == LED_FLASH_PERMANENT))) {
                // Flash MSD LED ONCE
                if (msd_led_value) {
                    msd_led_value = 0;
                } else {
                    msd_led_value = 1; // Turn on
                    if (msd_led_state == LED_FLASH) {
                        msd_led_usb_activity = 0;
                    }
                }

                // Update hardware
                gpio_set_msd_led(msd_led_value);
            }

            if (cdc_led_usb_activity && ((cdc_led_state == LED_FLASH) || (cdc_led_state == LED_FLASH_PERMANENT))) {
                // Flash CDC LED ONCE
                if (cdc_led_value) {
                    cdc_led_value = 0;
                } else {
                    cdc_led_value = 1; // Turn on
                    if (cdc_led_state == LED_FLASH) {
                        cdc_led_usb_activity = 0;
                    }
                }

                // Update hardware
                gpio_set_cdc_led(cdc_led_value);
            }

        }
    }
}

// Main Program
int main (void) {
  os_sys_init_user(main_task, MAIN_TASK_PRIORITY, stk_main_task, MAIN_TASK_STACK);
}
