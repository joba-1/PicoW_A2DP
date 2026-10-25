#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"

#include <btstack_run_loop.h>

#include "bt.h"
#include "a2dp.h"
#include "avrcp.h"
#include "volume.h"


// Unrecoverable error happened. Reboot by setting watchdog.
// Blink led until watchdog fires
// If RUN_PIN is used then try reset via run pin after 5 blinks
void fatal() {
    watchdog_enable(1000, true);  // reboot in 1s
    #if RUN_PIN >= 0
        unsigned count = 0;
    #endif
    while(true) {  // blink until reboot
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
        sleep_ms(20);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        sleep_ms(80);
        #if RUN_PIN >= 0
            if (++count >= 5) {
                // Pull run pin low, to reset the pico
                gpio_init(RUN_PIN);
                gpio_set_dir(RUN_PIN, GPIO_OUT);
                gpio_put(RUN_PIN, (count & 1) ? false : true);
            }
        #endif
    }
}


#if WATCHDOG_MS > 0
// reboot if the run loop stalls
static btstack_timer_source_t _watchdog_timer;
static bool _rebooting = false;

static void watchdog_feed( btstack_timer_source_t *ts ) {
    if (_rebooting) return;  // let the pending reboot happen
    watchdog_update();
    btstack_run_loop_set_timer(ts, WATCHDOG_MS / 4);
    btstack_run_loop_add_timer(ts);
}

static void watchdog_begin() {
    watchdog_enable(WATCHDOG_MS, true);  // pauses while debugging
    btstack_run_loop_set_timer_handler(&_watchdog_timer, watchdog_feed);
    watchdog_feed(&_watchdog_timer);
}
#endif


void on_bt_up( void *data ) {
    printf("Bluetooth stack is up\n");
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    #if WATCHDOG_MS > 0
        watchdog_begin();
    #endif
}


// led and pin show the connection, e.g. to switch the input of an AV receiver
void on_connection( bool connected, uint8_t *addr ) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, connected);
    #if CONN_PIN >= 0
        gpio_put(CONN_PIN, connected);
    #endif

    // only one source at a time: hide while connected, also saves radio time
    bt_discoverable(!connected);

    if (connected) {
        avrcp_connect_later(addr);  // needed for volume control
    }
    else {
        volume_save();
        #if REBOOT_ON_DISCONNECT
            printf("Reboot for a clean state\n");
            #if WATCHDOG_MS > 0
                _rebooting = true;
            #endif
            watchdog_reboot(0, 0, 100);
        #endif
    }
}


int main() {
    stdio_init_all();
    printf("PicoW A2DP " VERSION "\n");

    #if CONN_PIN >= 0
        // high while a source is connected
        gpio_init(CONN_PIN);
        gpio_set_dir(CONN_PIN, GPIO_OUT);
        gpio_put(CONN_PIN, 0);
    #endif

    // initialize CYW43 driver architecture (will enable BT if/because CYW43_ENABLE_BLUETOOTH == 1)
    if (cyw43_arch_init()) {
        printf("Failed to init cyw43_arch\n");
        fatal();
        return -1;
    }

    // led on during setup until bt is up
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);

    bt_begin(BT_NAME, BT_PIN, on_bt_up, NULL);
    a2dp_sink_begin(on_connection);
    avrcp_begin();
    volume_begin();

    printf("Setup done\n");
    bt_run();

    fatal();
    return -2;
}
