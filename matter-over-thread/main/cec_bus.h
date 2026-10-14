#pragma once

#include <stdint.h>

#include <driver/gpio.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// HDMI-CEC bit timing on one open-drain GPIO, run from a 50 us hardware timer interrupt.

#define CEC_BROADCAST 0xF
#define CEC_MAX_BLOCKS 16 // header + opcode + 14 operands

typedef struct {
    uint8_t blocks[CEC_MAX_BLOCKS]; // blocks[0] is the header: initiator << 4 | destination
    uint8_t length;                 // number of blocks, header included
} cec_frame_t;

typedef struct {
    enum : uint8_t { RECEIVED, SENT } type;
    bool ack; // SENT: acknowledged (directly addressed) or not rejected (broadcast)
    cec_frame_t frame;
} cec_event_t;

// Start the bus. Received frames and transmit results are posted to `events`.
esp_err_t cec_bus_init(gpio_num_t pin, QueueHandle_t events);

// Logical address this device acknowledges frames for, CEC_BROADCAST for none
void cec_bus_set_logical_address(uint8_t address);

// Queue a frame. Frames go out in order; one that is not acknowledged is retried twice.
bool cec_bus_send(const cec_frame_t *frame);
