#include "cec_bus.h"

#include <driver/gptimer.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>

static const char *TAG = "cec_bus";

// Everything below is counted in ticks of the timer interrupt
#define TICK_US 50
#define TICKS(us) ((us) / TICK_US)

// Nominal timing (HDMI 1.4b, CEC 5.2)
#define START_LOW TICKS(3700)
#define START_PERIOD TICKS(4500)
#define ONE_LOW TICKS(600)
#define ZERO_LOW TICKS(1500)
#define BIT_PERIOD TICKS(2400)
#define BIT_SAMPLE TICKS(1050)

// Low times accepted when receiving, with a tick of slack for interrupt latency
#define START_LOW_MIN (TICKS(3500) - 1)
#define START_LOW_MAX (TICKS(3900) + 1)
#define ONE_LOW_MIN (TICKS(400) - 1)
#define ONE_LOW_MAX (TICKS(800) + 1)
#define ZERO_LOW_MIN (TICKS(1300) - 1)
#define ZERO_LOW_MAX (TICKS(1700) + 1)

// Longest time from one falling edge to the next before a frame counts as cut off
#define START_TIMEOUT (TICKS(4700) + 2)
#define BIT_TIMEOUT (TICKS(2750) + 2)

// Signal free time before sending (CEC 9.1)
#define FREE_RETRY (3 * BIT_PERIOD)
#define FREE_NEW (5 * BIT_PERIOD)
#define FREE_NEXT (7 * BIT_PERIOD)

#define MAX_RETRIES 2
#define MAX_LOST_ARBITRATION 8

static gpio_num_t s_pin;
static QueueHandle_t s_events;
static QueueHandle_t s_transmit;
static volatile uint8_t s_address = CEC_BROADCAST;

static uint32_t s_now;
static bool s_line = true;
static uint32_t s_fall; // last falling edge
static uint32_t s_rise; // last rising edge, the line has been free since

// Receiver
static bool s_receiving;
static uint32_t s_rx_timeout;
static uint8_t s_rx_bit; // 0-7 data, 8 end of message, 9 acknowledge
static bool s_rx_eom;
static bool s_rx_direct; // directly addressed to us, so we acknowledge it
static bool s_rx_valid;  // every block so far was acknowledged as it should be
static bool s_rx_own;    // the frame on the bus is the one we are sending
static cec_frame_t s_rx;
static bool s_acking;
static uint32_t s_ack_release;

// Transmitter
static enum { TX_IDLE, TX_WAIT, TX_START, TX_BIT } s_tx_state;
static cec_frame_t s_tx;
static uint32_t s_tx_free = FREE_NEW;
static uint32_t s_tx_begin; // start of the current start bit or data bit
static uint8_t s_tx_block;
static uint8_t s_tx_bit;
static bool s_tx_value;
static bool s_tx_block_ok;
static uint8_t s_tx_retries;
static uint8_t s_tx_lost;

static inline void IRAM_ATTR pull_low(bool low)
{
    gpio_ll_set_level(&GPIO, s_pin, low ? 0 : 1);
}

static void IRAM_ATTR post(bool sent, bool ack, const cec_frame_t *frame, BaseType_t *woken)
{
    cec_event_t event = {};
    event.type = sent ? cec_event_t::SENT : cec_event_t::RECEIVED;
    event.ack = ack;
    event.frame = *frame;
    xQueueSendFromISR(s_events, &event, woken);
}

static void IRAM_ATTR on_fall()
{
    s_fall = s_now;
    if (!s_receiving) {
        return;
    }
    s_rx_timeout = s_now + BIT_TIMEOUT;
    if (s_rx_bit == 9 && s_rx_direct) {
        // Acknowledge by stretching the initiator's 1 into a 0
        pull_low(true);
        s_acking = true;
        s_ack_release = s_now + ZERO_LOW;
    }
}

static void IRAM_ATTR on_rise(BaseType_t *woken)
{
    uint32_t low = s_now - s_fall;
    s_rise = s_now;

    if (low >= START_LOW_MIN && low <= START_LOW_MAX) {
        s_receiving = true;
        s_rx_timeout = s_fall + START_TIMEOUT;
        s_rx_bit = 0;
        s_rx_direct = false;
        s_rx_valid = true;
        s_rx_own = s_tx_state == TX_START;
        s_rx.length = 0;
        s_rx.blocks[0] = 0;
        return;
    }
    if (!s_receiving) {
        return;
    }

    bool bit;
    if (low >= ONE_LOW_MIN && low <= ONE_LOW_MAX) {
        bit = true;
    } else if (low >= ZERO_LOW_MIN && low <= ZERO_LOW_MAX) {
        bit = false;
    } else {
        s_receiving = false;
        return;
    }

    if (s_rx_bit < 8) {
        s_rx.blocks[s_rx.length] = (s_rx.blocks[s_rx.length] << 1) | bit;
        if (s_rx_bit == 7 && s_rx.length == 0) {
            uint8_t destination = s_rx.blocks[0] & 0xF;
            s_rx_direct = !s_rx_own && destination != CEC_BROADCAST && destination == s_address;
        }
    } else if (s_rx_bit == 8) {
        s_rx_eom = bit;
    } else {
        // A 0 acknowledges a directly addressed block but rejects a broadcast one
        bool broadcast = (s_rx.blocks[0] & 0xF) == CEC_BROADCAST;
        if (bit == broadcast) {
            s_rx_valid = false;
        }
        s_rx.length++;
        if (s_rx_eom || s_rx.length == CEC_MAX_BLOCKS) {
            s_receiving = false;
            if (s_rx_eom && s_rx_valid && !s_rx_own) {
                post(false, true, &s_rx, woken);
            }
            return;
        }
        s_rx.blocks[s_rx.length] = 0;
    }
    s_rx_bit = (s_rx_bit + 1) % 10;
}

static bool IRAM_ATTR tx_bit_value()
{
    if (s_tx_bit < 8) {
        return (s_tx.blocks[s_tx_block] >> (7 - s_tx_bit)) & 1;
    }
    if (s_tx_bit == 8) {
        return s_tx_block == s_tx.length - 1;
    }
    return true; // the acknowledge bit is sent as 1 and pulled to 0 by the follower
}

static void IRAM_ATTR tx_begin_bit()
{
    s_tx_begin = s_now;
    s_tx_value = tx_bit_value();
    s_tx_state = TX_BIT;
    pull_low(true);
}

static void IRAM_ATTR tx_finish(bool ack, BaseType_t *woken)
{
    post(true, ack, &s_tx, woken);
    s_tx_free = FREE_NEXT;
    s_tx_state = TX_IDLE;
}

static void IRAM_ATTR tx_retry(BaseType_t *woken)
{
    if (++s_tx_retries > MAX_RETRIES) {
        tx_finish(false, woken);
        return;
    }
    s_tx_free = FREE_RETRY;
    s_tx_state = TX_WAIT;
}

static void IRAM_ATTR transmit(BaseType_t *woken)
{
    uint32_t elapsed = s_now - s_tx_begin;
    switch (s_tx_state) {
    case TX_IDLE:
        if (xQueueReceiveFromISR(s_transmit, &s_tx, woken) == pdTRUE) {
            s_tx_retries = 0;
            s_tx_lost = 0;
            s_tx_state = TX_WAIT;
        }
        break;

    case TX_WAIT:
        if (s_line && !s_receiving && !s_acking && s_now - s_rise >= s_tx_free) {
            s_tx_begin = s_now;
            s_tx_state = TX_START;
            pull_low(true);
        }
        break;

    case TX_START:
        if (elapsed == START_LOW) {
            pull_low(false);
        } else if (elapsed >= START_PERIOD) {
            s_tx_block = 0;
            s_tx_bit = 0;
            s_tx_block_ok = true;
            tx_begin_bit();
        }
        break;

    case TX_BIT:
        if (elapsed == (s_tx_value ? ONE_LOW : ZERO_LOW)) {
            pull_low(false);
        } else if (elapsed == BIT_SAMPLE && s_tx_value) {
            bool low = !s_line;
            if (s_tx_bit == 9) {
                bool broadcast = (s_tx.blocks[0] & 0xF) == CEC_BROADCAST;
                s_tx_block_ok = low != broadcast;
            } else if (low) {
                // Someone else pulled the line low: lost arbitration, try again once the bus is free
                s_rx_own = false;
                if (++s_tx_lost > MAX_LOST_ARBITRATION) {
                    tx_finish(false, woken);
                } else {
                    s_tx_free = FREE_NEW;
                    s_tx_state = TX_WAIT;
                }
            }
        } else if (elapsed >= BIT_PERIOD) {
            if (s_tx_bit < 9) {
                s_tx_bit++;
                tx_begin_bit();
            } else if (!s_tx_block_ok) {
                tx_retry(woken);
            } else if (s_tx_block == s_tx.length - 1) {
                tx_finish(true, woken);
            } else {
                s_tx_block++;
                s_tx_bit = 0;
                tx_begin_bit();
            }
        }
        break;
    }
}

static bool IRAM_ATTR on_tick(gptimer_handle_t timer, const gptimer_alarm_event_data_t *data, void *arg)
{
    BaseType_t woken = pdFALSE;
    s_now++;

    bool line = gpio_ll_get_level(&GPIO, s_pin);
    if (line != s_line) {
        s_line = line;
        if (line) {
            on_rise(&woken);
        } else {
            on_fall();
        }
    }
    if (s_receiving && s_line && (int32_t)(s_now - s_rx_timeout) > 0) {
        s_receiving = false;
    }
    if (s_acking && (int32_t)(s_now - s_ack_release) >= 0) {
        pull_low(false);
        s_acking = false;
    }
    transmit(&woken);
    return woken == pdTRUE;
}

esp_err_t cec_bus_init(gpio_num_t pin, QueueHandle_t events)
{
    s_pin = pin;
    s_events = events;
    s_transmit = xQueueCreate(16, sizeof(cec_frame_t));
    ESP_RETURN_ON_FALSE(s_transmit, ESP_ERR_NO_MEM, TAG, "no memory for the transmit queue");

    // Open drain: 0 pulls the line low, 1 releases it to the pull-up
    gpio_reset_pin(pin);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "configure CEC GPIO");
    gpio_set_level(pin, 1);

    gptimer_handle_t timer;
    gptimer_config_t timer_config = {};
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = 1000000;
    timer_config.intr_priority = 3;
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer_config, &timer), TAG, "create timer");

    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = TICK_US;
    alarm.flags.auto_reload_on_alarm = true;
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(timer, &alarm), TAG, "set timer alarm");

    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = on_tick;
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(timer, &callbacks, nullptr), TAG, "register timer callback");
    ESP_RETURN_ON_ERROR(gptimer_enable(timer), TAG, "enable timer");
    return gptimer_start(timer);
}

void cec_bus_set_logical_address(uint8_t address)
{
    s_address = address;
}

bool cec_bus_send(const cec_frame_t *frame)
{
    return xQueueSend(s_transmit, frame, 0) == pdTRUE;
}
