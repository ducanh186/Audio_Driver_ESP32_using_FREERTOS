#pragma once
#pragma GCC diagnostic ignored "-Wformat"
#include <freertos/FreeRTOS.h>
#include <driver/i2s.h>
#include "driver/gpio.h"

// sample rate
#define SAMPLE_RATE 44100

// I2S Speaker Settings (MAX98357A)
#define I2S_SPEAKER_SERIAL_CLOCK      GPIO_NUM_17  // BCLK → MAX98357 BCLK
#define I2S_SPEAKER_LEFT_RIGHT_CLOCK  GPIO_NUM_18  // LRCLK → MAX98357 LRC
#define I2S_SPEAKER_SERIAL_DATA       GPIO_NUM_21  // DIN → MAX98357 DIN
// Pause, Next, Back buttons
#define BTN_PAUSE_GPIO GPIO_NUM_5
#define BTN_NEXT_GPIO  GPIO_NUM_6
#define BTN_BACK_GPIO  GPIO_NUM_7
// I2S speaker config
extern i2s_config_t i2s_speaker_config;
extern i2s_pin_config_t i2s_speaker_pins;
