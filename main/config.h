#pragma once

#include <freertos/FreeRTOS.h>
#include <driver/i2s.h>

// sample rate
#define SAMPLE_RATE 44100

// I2S Speaker Settings (MAX98357A)
#define I2S_SPEAKER_SERIAL_CLOCK      GPIO_NUM_17  // BCLK → MAX98357 BCLK
#define I2S_SPEAKER_LEFT_RIGHT_CLOCK  GPIO_NUM_18  // LRCLK → MAX98357 LRC
#define I2S_SPEAKER_SERIAL_DATA       GPIO_NUM_21  // DIN → MAX98357 DIN

// I2S speaker config
extern i2s_config_t i2s_speaker_config;
extern i2s_pin_config_t i2s_speaker_pins;
