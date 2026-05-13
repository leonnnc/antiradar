#pragma once

#include <Arduino.h>

// Shared SPI bus: TFT + nRF24 radios
static constexpr uint8_t CD_SPI_SCK = 12;
static constexpr uint8_t CD_SPI_MOSI = 11;
static constexpr uint8_t CD_SPI_MISO = 13;

// TFT ST7789
static constexpr uint8_t CD_TFT_CS = 10;
static constexpr uint8_t CD_TFT_DC = 21;
static constexpr uint8_t CD_TFT_RST = 14;
static constexpr int8_t CD_TFT_BL = -1;

// Dual nRF24L01
static constexpr uint8_t CD_NRF1_CE = 4;
static constexpr uint8_t CD_NRF1_CSN = 5;
static constexpr uint8_t CD_NRF2_CE = 6;
static constexpr uint8_t CD_NRF2_CSN = 7;

// Dedicated microSD SPI bus
static constexpr uint8_t CD_SD_SCK = 36;
static constexpr uint8_t CD_SD_MOSI = 35;
static constexpr uint8_t CD_SD_MISO = 37;
static constexpr uint8_t CD_SD_CS = 16;

// GPS NEO-6M on UART1
static constexpr uint8_t CD_GPS_RX = 18;
static constexpr uint8_t CD_GPS_TX = 17;
static constexpr uint32_t CD_GPS_BAUD = 9600;

// Buttons, active LOW
static constexpr uint8_t CD_BTN_UP = 1;
static constexpr uint8_t CD_BTN_DOWN = 2;
static constexpr uint8_t CD_BTN_OK = 42;
static constexpr uint8_t CD_BTN_BACK = 41;

// Rotary encoder, active LOW
static constexpr uint8_t CD_ENC_CLK = 40;
static constexpr uint8_t CD_ENC_DT = 39;
static constexpr uint8_t CD_ENC_SW = 38;

// Buzzer and battery
static constexpr uint8_t CD_BUZZER = 15;
static constexpr uint8_t CD_VBAT_ADC = 9;

// Battery divider: 2.2k from VBAT to ADC, 1k from ADC to GND.
static constexpr float CD_VBAT_DIVIDER = 3.2f;
