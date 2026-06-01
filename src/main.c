// SPDX-License-Identifier: MIT
//
// PS/2 TrackPoint reader  —  ESP32-C6
// ----------------------------------------------------------------------------
// Reads an IBM/Lenovo TrackPoint module (Philips PTPM754, a PS/2 mouse) and
// prints decoded movement + buttons to the serial console.
//
// Wiring (pads identified with a multimeter against the PTPM754 pinout):
//   CLK  -> GPIO1   (PS/2 clock, device-driven; we ISR on its falling edge)
//   DATA -> GPIO7   (PS/2 data)
//   RST  -> GPIO5   (active-HIGH reset; pulse HIGH then LOW to re-init)
//   VCC  -> 3V3     (PTPM754 is 2.7-6 V; use 3V3 so the OC lines idle at 3.3 V)
//   GND  -> GND     (and the module shield)
//
// PS/2 frame (device->host): 1 start(0), 8 data LSB-first, 1 odd-parity, 1 stop(1).
// Host samples DATA on the FALLING edge of CLK. After reset the mouse is in
// stream mode but reporting is DISABLED, so we must send 0xF4 to start packets.
// Console is UART0 via the CH343 bridge (see sdkconfig.defaults / README).

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define PIN_CLK  GPIO_NUM_1
#define PIN_DATA GPIO_NUM_7
#define PIN_RST  GPIO_NUM_5

static const char *TAG = "tp";

// ---- RX: CLK-falling-edge ISR assembles 11-bit frames into a byte queue -----
static QueueHandle_t s_rx;
static volatile int      s_bit  = 0;   // 0=start, 1..8=data, 9=parity, 10=stop
static volatile uint32_t s_data = 0;
static volatile int      s_par  = 0;
static volatile int64_t  s_last = 0;

static void IRAM_ATTR clk_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last > 200) s_bit = 0;   // >200us gap => start of a new frame
    s_last = now;

    int b = gpio_get_level(PIN_DATA);
    if (s_bit == 0) {
        if (b != 0) return;              // start bit must be 0
        s_data = 0; s_par = 0; s_bit = 1;
    } else if (s_bit <= 8) {
        if (b) { s_data |= (1u << (s_bit - 1)); s_par ^= 1; }
        s_bit++;
    } else if (s_bit == 9) {
        s_par ^= b;                      // include parity bit -> should be odd(=1)
        s_bit++;
    } else {                             // stop bit
        uint8_t byte = s_data & 0xFF;
        BaseType_t hpw = pdFALSE;
        xQueueSendFromISR(s_rx, &byte, &hpw);
        s_bit = 0;
        if (hpw) portYIELD_FROM_ISR();
    }
}

static bool rx_byte(uint8_t *out, int timeout_ms)
{
    return xQueueReceive(s_rx, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// ---- TX: host->device byte (bit-banged, with timeouts so we never hang) -----
static bool clk_wait(int level, int to_us)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_CLK) != level)
        if (esp_timer_get_time() - t0 > to_us) return false;
    return true;
}

static bool ps2_write(uint8_t data)
{
    gpio_intr_disable(PIN_CLK);

    // Request-to-send: hold CLK low >=100us, then pull DATA low (start bit).
    gpio_set_level(PIN_CLK, 0);
    esp_rom_delay_us(150);
    gpio_set_level(PIN_DATA, 0);
    esp_rom_delay_us(10);
    gpio_set_level(PIN_CLK, 1);          // release CLK -> device clocks us

    bool ok = true;
    uint8_t parity = 1;                  // odd parity
    for (int i = 0; i < 8 && ok; i++) {
        int bit = (data >> i) & 1;
        if (!clk_wait(0, 3000)) { ok = false; break; }   // device pulls CLK low
        gpio_set_level(PIN_DATA, bit);                   // set data while CLK low
        parity ^= bit;
        if (!clk_wait(1, 3000)) { ok = false; break; }   // device samples on rise
    }
    if (ok && clk_wait(0, 3000)) gpio_set_level(PIN_DATA, parity); else ok = false;
    if (ok && !clk_wait(1, 3000)) ok = false;
    if (ok && clk_wait(0, 3000)) gpio_set_level(PIN_DATA, 1); else ok = false; // stop: release
    if (ok && !clk_wait(1, 3000)) ok = false;
    if (ok) {                            // device ACK: pulls DATA low
        int64_t t0 = esp_timer_get_time(); bool ack = false;
        while (esp_timer_get_time() - t0 < 3000)
            if (gpio_get_level(PIN_DATA) == 0) { ack = true; break; }
        if (!ack) ok = false;
        clk_wait(1, 3000);               // wait for idle
    }

    gpio_set_level(PIN_DATA, 1);
    gpio_set_level(PIN_CLK, 1);
    s_bit = 0; s_last = esp_timer_get_time();
    xQueueReset(s_rx);
    gpio_intr_enable(PIN_CLK);
    return ok;
}

static bool ps2_cmd(uint8_t cmd, const char *name)
{
    if (!ps2_write(cmd)) { ESP_LOGW(TAG, "%s (0x%02X): no clock from device", name, cmd); return false; }
    uint8_t r;
    if (!rx_byte(&r, 300)) { ESP_LOGW(TAG, "%s (0x%02X): no ACK", name, cmd); return false; }
    ESP_LOGI(TAG, "%s (0x%02X) -> 0x%02X %s", name, cmd, r, r == 0xFA ? "(ACK)" : "(unexpected!)");
    return r == 0xFA;
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "PS/2 TrackPoint reader: CLK=GPIO%d DATA=GPIO%d RST=GPIO%d",
             PIN_CLK, PIN_DATA, PIN_RST);

    // CLK + DATA: open-drain (we only ever pull low or release), pull-ups on.
    gpio_config_t bus = {
        .pin_bit_mask = (1ULL << PIN_CLK) | (1ULL << PIN_DATA),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bus);
    gpio_set_level(PIN_CLK, 1);
    gpio_set_level(PIN_DATA, 1);

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gpio_set_level(PIN_RST, 0);           // released (run)

    s_rx = xQueueCreate(64, sizeof(uint8_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_CLK, clk_isr, NULL);
    gpio_set_intr_type(PIN_CLK, GPIO_INTR_NEGEDGE);

    // Hardware reset -> device runs BAT and emits 0xAA 0x00.
    ESP_LOGI(TAG, "Resetting module via RST (HIGH 30ms -> LOW)...");
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    xQueueReset(s_rx);
    s_bit = 0; s_last = esp_timer_get_time();
    gpio_set_level(PIN_RST, 0);

    uint8_t b;
    if (rx_byte(&b, 1000))
        ESP_LOGI(TAG, "BAT byte 1: 0x%02X %s", b, b == 0xAA ? "(self-test OK)" : "(expected 0xAA!)");
    else
        ESP_LOGW(TAG, "No BAT after reset. Check VCC(3V3)/GND/shield and CLK=GPIO%d wiring.", PIN_CLK);
    if (rx_byte(&b, 400))
        ESP_LOGI(TAG, "BAT byte 2 (device id): 0x%02X %s", b, b == 0x00 ? "(mouse)" : "");

    // Optional: explicit reset command, then enable data reporting.
    ps2_cmd(0xFF, "RESET");
    rx_byte(&b, 400); rx_byte(&b, 100);   // swallow any 0xAA/0x00 from 0xFF
    bool en = ps2_cmd(0xF4, "ENABLE-REPORT");
    if (!en) ESP_LOGW(TAG, "Reporting not enabled — no packets will arrive. See warnings above.");

    ESP_LOGI(TAG, "Streaming. Move the nub / press buttons. (idle = no packets, that's normal)");
    xQueueReset(s_rx);

    uint8_t pkt[3]; int idx = 0; int64_t last_msg = esp_timer_get_time();
    while (1) {
        if (!rx_byte(&b, 1000)) {
            if (esp_timer_get_time() - last_msg > 5000000) {
                ESP_LOGI(TAG, "...alive, waiting for movement...");
                last_msg = esp_timer_get_time();
            }
            continue;
        }
        if (idx == 0 && !(b & 0x08)) continue;   // first packet byte always has bit3=1; resync
        pkt[idx++] = b;
        if (idx < 3) continue;
        idx = 0;

        bool L = pkt[0] & 0x01, R = pkt[0] & 0x02, M = pkt[0] & 0x04;
        int dx = (int)pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
        int dy = (int)pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);
        ESP_LOGI(TAG, "dx=%4d dy=%4d   L=%d M=%d R=%d", dx, dy, L, M, R);
        last_msg = esp_timer_get_time();
    }
}
