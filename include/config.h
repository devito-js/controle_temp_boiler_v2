#pragma once
// Pinagem provisoria: confirmar na placa TENSTAR antes de conectar.
constexpr int RELAY_PIN = 27;
constexpr int NTC_PIN = 32;
constexpr bool RELAY_ACTIVE_LOW = true;
constexpr float SERIES_OHMS = 10000.0f;
constexpr float NTC_OHMS = 10000.0f;
constexpr float NTC_BETA = 3950.0f;
constexpr float SUPPLY_MV = 3300.0f;
constexpr float MAX_TEMP = 75.0f;
constexpr float HYSTERESIS = 2.0f;
// Botoes integrados da T-Display classica (ativos em LOW).
constexpr int TEMP_UP_PIN = 35;
constexpr int TEMP_DOWN_PIN = 0;
constexpr float TEMP_STEP = 0.5f;
constexpr uint32_t MODE_HOLD_MS = 2000;
constexpr uint32_t DISPLAY_TIMEOUT_MS = 20000;
constexpr uint8_t DISPLAY_BRIGHTNESS = 255; // 0..255
constexpr uint8_t DISPLAY_IDLE_BRIGHTNESS = 8; // 0 apaga; ex.: 8 fica fraco
constexpr char AP_NAME[] = "Boiler-Config";
constexpr char AP_PASSWORD[] = "boiler123"; // Alterar antes do uso.pode ser
