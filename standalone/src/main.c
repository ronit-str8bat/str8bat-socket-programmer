#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#if STANDALONE_NETWORK
#include "pico/flash.h"
#include "services/network.h"
#if STANDALONE_LOCAL_AP
#include "services/local_api.h"
#else
#include "services/usb_export.h"
#endif
#endif
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"
#include "programmer_config.h"
#include "firmware_image.h"
#include "crc32.h"
#include "ui_logic.h"
#include "swd.h"
#include "stm32_programmer.h"
#if STANDALONE_USB_DIAGNOSTICS
#include "pico/stdio_usb.h"
#include <stdio.h>
#include <inttypes.h>
#endif

/* Core 0 owns CYW43/onboard LED, button and RGB. Core 1 alone owns SWD.
 * Semaphore publication protects the final report. Live progress fields
 * are debugger diagnostics only; the UI does not rely on consistent snapshots.
 */
volatile programmer_report_t last_report;
volatile programmer_stage_t active_stage;
volatile uint32_t bytes_completed;
static semaphore_t job_requested, job_finished, worker_ready;
static const uint8_t *job_image;
static uint32_t job_size, job_crc;
static bool worker_ok = true;

static bool pin_map_valid(void) {
    const int pins[] = {SWCLK_PIN, SWDIO_PIN, NRST_PIN, START_PIN, SWD_OE_PIN,
                       RED_LED_PIN, GREEN_LED_PIN, BLUE_LED_PIN, STATUS_LED_PIN};
    for (unsigned i = 0; i < sizeof(pins)/sizeof(pins[0]); ++i) {
        /* Only GPIOs exposed on the Pico header; no radio/flash power pins. */
        if (!(pins[i] >= 0 && (pins[i] <= 22 || (pins[i] >= 26 && pins[i] <= 28)))) return false;
        for (unsigned j = 0; j < i; ++j) if (pins[i] == pins[j]) return false;
    }
    return true;
}
static void rgb(bool red, bool green, bool blue) {
    /* Keep logical colors/patterns identical; invert only the output levels. */
    gpio_put(RED_LED_PIN, RGB_LED_COMMON_ANODE ? !red : red);
    gpio_put(GREEN_LED_PIN, RGB_LED_COMMON_ANODE ? !green : green);
    gpio_put(BLUE_LED_PIN, RGB_LED_COMMON_ANODE ? !blue : blue);
}
static bool error_led_on(unsigned code, uint32_t elapsed_ms) {
    /* Result enum 1..9: countable 250 ms pulses, then a 2-second gap.
     * Use the onboard LED so the requested solid-red RGB result is preserved.
     */
    uint32_t pulse_window = code * 500u;
    uint32_t phase = elapsed_ms % (pulse_window + 2000u);
    return phase < pulse_window && phase % 500u < 250u;
}
static void update_status_led(uint32_t now) {
#if STANDALONE_LOCAL_AP
    bool on = status_led_on(local_logs_full(), local_time_valid(),
                            local_wifi_connected(), now);
    gpio_put(STATUS_LED_PIN, STATUS_LED_COMMON_ANODE ? !on : on);
#else
    (void)now;
#endif
}
static void progress(programmer_stage_t stage, uint32_t done) {
    active_stage = stage;
    bytes_completed = done;
}
static void worker(void) {
#if STANDALONE_NETWORK
    worker_ok = flash_safe_execute_core_init();
#endif
    swd_init();
    sem_release(&worker_ready);
    while (true) {
        sem_acquire_blocking(&job_requested);
        /* Check XIP image integrity before every erase, not just at boot. */
        if (image_crc32(job_image, job_size) != job_crc) {
            last_report = (programmer_report_t){ .result = RESULT_IMAGE };
        } else {
            last_report = stm32_program(job_image, job_size, progress);
        }
        __dmb();
        sem_release(&job_finished);
    }
}

int main(void) {
    if (!pin_map_valid()) { /* Invalid pin maps must not drive arbitrary pins. */
        while (true) tight_loop_contents();
    }
    const unsigned led_pins[] = {RED_LED_PIN, GREEN_LED_PIN, BLUE_LED_PIN};
    for (unsigned i = 0; i < 3; ++i) {
        gpio_init(led_pins[i]);
        gpio_put(led_pins[i], RGB_LED_COMMON_ANODE ? 1 : 0); /* Preload OFF. */
        gpio_set_dir(led_pins[i], GPIO_OUT);
    }
    gpio_init(STATUS_LED_PIN);
    gpio_put(STATUS_LED_PIN, STATUS_LED_COMMON_ANODE ? 1 : 0); /* Preload OFF. */
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
    gpio_init(START_PIN);
    gpio_set_dir(START_PIN, GPIO_IN);
    gpio_pull_up(START_PIN);
    /* Early safety: no driven SWD pins while socket is empty or CRC is checked. */
    swd_init();
    bool onboard_ok = cyw43_arch_init() == 0;
    if (onboard_ok) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    bool image_ok = image_crc32(firmware_image, firmware_image_size) == firmware_image_crc32;
#if STANDALONE_USB_DIAGNOSTICS
    stdio_init_all(); /* Configured to wait zero ms; no host is required. */
#endif
    sem_init(&job_requested, 0, 1);
    sem_init(&job_finished, 0, 1);
    sem_init(&worker_ready, 0, 1);
    multicore_launch_core1(worker);
    sem_acquire_blocking(&worker_ready);
    image_ok = image_ok && worker_ok;
#if STANDALONE_NETWORK
    if (image_ok && onboard_ok) image_ok = services_init();
#endif
    uint32_t self_test_finished = to_ms_since_boot(get_absolute_time());
    if (!onboard_ok || !image_ok) {
        /* Failed startup/checksum blinks red and blocks START. */
#if STANDALONE_NETWORK && STANDALONE_USB_DIAGNOSTICS
        uint32_t last_startup_report = 0;
#endif
        while (true) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
#if STANDALONE_NETWORK && STANDALONE_USB_DIAGNOSTICS
            if (stdio_usb_connected() && (uint32_t)(now - last_startup_report) >= 2000u) {
                last_startup_report = now;
                printf("PICO-PROGRAMMER " PROGRAMMER_BUILD_ID " STARTUP_FAILED network=%s onboard=%u checks=%u worker=%u\n",
                       services_status(), (unsigned)onboard_ok, (unsigned)image_ok, (unsigned)worker_ok);
            }
#endif
            rgb(((now - self_test_finished) / STATUS_BLINK_HALF_MS) % 2 == 0, false, false);
            if (onboard_ok) cyw43_arch_poll();
            update_status_led(now);
            sleep_ms(1);
        }
    }
    rgb(false, true, false);
    start_button_t button;
    start_button_reset(&button, !gpio_get(START_PIN), to_ms_since_boot(get_absolute_time()));
    bool busy = false;
    bool waiting_for_first_start = true;
    unsigned error_code = 0;
    uint32_t error_started = 0;
#if STANDALONE_USB_DIAGNOSTICS
    uint32_t last_usb_report = 0;
#endif
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bool pressed = !gpio_get(START_PIN);
        if (busy) {
            rgb(false, false, true);
            /* Busy presses are discarded; a held button never queues a job. */
            if (sem_try_acquire(&job_finished)) {
                __dmb();
                bool ok = last_report.result == RESULT_OK;
                error_code = (unsigned)last_report.result;
#if STANDALONE_NETWORK
                programmer_report_t completed_report = last_report;
                if (!services_job_finish(&completed_report)) { ok = false; error_code = 10; }
#endif
                error_started = now;
                rgb(!ok, ok, false);
                busy = false;
                start_button_reset(&button, pressed, now);
            }
        } else if (start_button_update(&button, pressed, now)) {
#if STANDALONE_NETWORK
            if (!services_job_begin()) {
                waiting_for_first_start = false;
                error_code = 10; error_started = now;
#if STANDALONE_LOCAL_AP
                error_code = local_start_error();
#endif
                rgb(true, false, false);
                start_button_reset(&button, pressed, now);
                services_poll(false);
                update_status_led(now);
                continue;
            }
            stored_image_t image = services_program_image();
            job_image = image.data; job_size = image.size; job_crc = image.crc;
#else
            job_image = firmware_image; job_size = firmware_image_size; job_crc = firmware_image_crc32;
#endif
            busy = true;
            waiting_for_first_start = false;
            error_code = 0;
            rgb(false, false, true);
            sem_release(&job_requested);
        } else if (waiting_for_first_start) {
            /* LED 1: checksum/startup passed. Wi-Fi is indicated by LED 2.
             * Completed-job green/red results remain solid. */
            rgb(false, ((now - self_test_finished) / STATUS_BLINK_HALF_MS) % 2 == 0, false);
        }
#if STANDALONE_NETWORK && STANDALONE_LOCAL_AP
        /* A factory BIN transfer has priority over the idle indication. The
         * three colours rotate while the master distributes or a slave receives. */
        if(!busy && services_factory_led_active()) {
            unsigned phase=(now/150u)%3u;
            rgb(phase==0u,phase==1u,phase==2u);
        }
#endif
#if STANDALONE_NETWORK && !STANDALONE_LOCAL_AP
        usb_export_poll(busy);
#endif
        /* Failures give a repeating error count; otherwise an activity heartbeat. */
        bool onboard_on = error_code ? error_led_on(error_code, now - error_started)
                                     : (now / (busy ? 125u : 500u)) % 2;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, onboard_on);
#if STANDALONE_USB_DIAGNOSTICS
        /* Only read the published report while the worker is idle. Repeating
         * it lets a PC connect AFTER the failure. Network builds also accept
         * the read-only CSV export protocol; USB cannot start programming.
         * Core 1 never formats/prints, so no USB waits occur in SWD transfers.
         */
        if (!busy && stdio_usb_connected() && (uint32_t)(now - last_usb_report) >= 2000u
#if STANDALONE_NETWORK && !STANDALONE_LOCAL_AP
            && !usb_export_active()
#endif
        ) {
            last_usb_report = now;
#if STANDALONE_NETWORK
            printf("NETWORK status=%s pico=%s image_revision=%lu image_bytes=%lu\n",
                   services_status(), storage_device_id(), (unsigned long)storage_image().revision,
                   (unsigned long)storage_image().size);
#endif
            if (waiting_for_first_start) {
                printf("PICO-PROGRAMMER " PROGRAMMER_BUILD_ID " READY block=16384 fast=%u halt_after_verify=%u\n",
                       (unsigned)STM32_USE_FAST_PROGRAM, (unsigned)STM32_HALT_AFTER_VERIFY);
            } else {
                programmer_report_t r = last_report;
                printf("PICO-PROGRAMMER " PROGRAMMER_BUILD_ID " result=%u stage=%u address=%08" PRIx32
                       " detail=%08" PRIx32 " loader=%" PRIu32 " completed=%" PRIu32
                       " retry=%" PRIu32 " fast_rows=%" PRIu32 " elapsed_ms=%" PRIu32 " halted=%" PRIu32 "\n",
                       (unsigned)r.result, (unsigned)r.stage, r.address, r.detail,
                       r.loader_status, r.loader_completed, r.standard_retry, r.fast_rows, r.elapsed_ms, r.target_halted);
                printf("FAST-FIRST address=%08" PRIx32 " detail=%08" PRIx32 " loader=%" PRIu32
                       " STAGES_MS=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                       r.fast_error_address, r.fast_error_detail, r.fast_error_status,
                       r.stage_ms[0], r.stage_ms[1], r.stage_ms[2], r.stage_ms[3], r.stage_ms[4]);
                printf("OPTIONS before=%08" PRIx32 " after=%08" PRIx32
                       " bfb2_attempted=%" PRIu32 " bfb2_cleared=%" PRIu32 " phase=%" PRIu32
                       " dhcsr=%08" PRIx32 " reconnects=%" PRIu32 "\n",
                       r.optr_before, r.optr_after, r.bfb2_attempted, r.bfb2_cleared, r.option_phase,
                       r.reload_dhcsr, r.reload_reconnects);
            }
        }
#endif
#if STANDALONE_NETWORK
        services_poll(busy);
#else
        cyw43_arch_poll();
#endif
        update_status_led(now);
        sleep_ms(1);
    }
}
