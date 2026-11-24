# ESP32 Intex PureSpa SB-H20 WiFi Controller (Hardened Fork — v1.5.3)

> This is a stability-hardened and feature-extended fork of [`caspercze/esp32-intexsbh20`](https://github.com/caspercze/esp32-intexsbh20) (v1.4.0), designed for reliable 24/7 outdoor operation directly on the Intex PureSpa 5 V display bus and deep integration with Home Assistant.

## Credits

- **ESP32 port & v1.4.0 base** by Petr Kašpar: [caspercze/esp32-intexsbh20](https://github.com/caspercze/esp32-intexsbh20)
- **Original ESP8266 project** by Jens B.: [jnsbyr/esp8266-intexsbh20](https://github.com/jnsbyr/esp8266-intexsbh20)
- **Intex bus protocol / DIYSCIP** by Geoffroy Hubert: [yorffoeg/diyscip](https://github.com/yorffoeg/diyscip)

---

## What's New & Fixed in v1.5.3 (compared to upstream v1.4.0)

### 1. 100 kHz Bus ISR & Flash-Cache Hardening
- **100% IRAM-resident ISR call chain (`IRAM_ATTR`):** `clockRisingISR()`, `decodeDisplay()`, `decodeLED()`, `decodeButton()`, `updateButtonState()`, `displayTempToCelsiusRaw()`, `shouldRejectBlinkAsSetpoint()`, `display2Num()`, and all helper functions now reside strictly in `.iram0.text` (verified via `xtensa-esp32-elf-objdump` — **0 flash accesses, 0 FPU instructions** inside the 100 kHz interrupt).
- **No floating-point math in ISR:** Replaced `float` / `round()` Fahrenheit conversion in `displayTempToCelsiusRaw()` with exact integer rounding.
- **ESP-IDF 5.x register-direct open-drain GPIO (`GPIO19`):** Configures `GPIO_MODE_INPUT_OUTPUT_OD` once during `PureSpaIO::setup()`, then performs all ISR pin reads (`GPIO.in`) and 3 µs open-drain button ACK pulses (`GPIO.out_w1tc` / `GPIO.enable_w1ts`) directly via hardware registers instead of flash-resident `digitalRead()` / `gpio_set_direction()`.
- **Safe flash writes (`pauseBusISR` / `resumeBusISR`):** Automatically pauses the 100 kHz clock interrupt during Web-OTA, HTTP-OTA, and EEPROM config saves to prevent flash-cache contention.

### 2. Dual-Core Hardware Watchdog & RTC Crash Blackbox
- **30-second Dual-Core Task Watchdog (`esp_task_wdt`):** Monitors both **Core 0** (Wi-Fi/LwIP IDLE task) and **Core 1** (`loopTask`) with `trigger_panic = true`, automatically recovering from any hang instead of locking up until a manual power cycle. All blocking button/command wait loops, MQTT reconnects, and OTA chunk handlers feed the watchdog.
- **Persistent `RTC_NOINIT_ATTR` Crash Blackbox:** Survives warm reboots, watchdog resets, OTA updates, and brownouts in RTC RAM to preserve:
  - `boot_count`
  - `reset_reason` (current boot reason: `POWERON`, `SW_RESTART`, `BROWNOUT`, `TASK_WDT`, `PANIC`, etc.)
  - `last_crash_reason` & `last_crash_uptime_s` (last abnormal reset never gets overwritten by a subsequent normal restart)
  - `savedSetTempC` (authoritative target temperature preserved across warm reboots without pressing buttons on the panel)

### 3. Power & Brownout Elimination for the Intex 5 V Display Bus
Running a full-power ESP32 (`240 MHz`, `19.5 dBm` TX, `WiFi.setSleep(false)`) directly from the Intex control panel's 5 V rail (especially via a NodeMCU board with an `AMS1117-3.3` LDO that has a `1.1–1.3 V` dropout voltage) can cause the `3.3 V` rail to dip briefly during Wi-Fi RF bursts when the 2.2 kW heater relay is energized, triggering `BROWNOUT` resets and bootloops during `WiFi.mode(WIFI_STA)` RF calibration.
- **Stage A — Disarmed Hair-Trigger `2.43 V` Brownout Interrupt (`RTC_CNTL_BROWN_OUT_REG = 0`):** Suppresses false software brownout-interrupt resets caused by microsecond Wi-Fi RF pulses behind the `AMS1117-3.3` LDO while keeping the 30-second Dual-Core Hardware Task Watchdog 100% active.
- **Stage B — Automatic Local-Bus Heater Pause & Resume on Recovery Boot:** On any abnormal/brownout reboot (`bootCount > 1` or repeated Wi-Fi startup attempt), the ESP32 inspects the local 100 kHz Intex bus (`GPIO 18/19/23`) **before** turning on the Wi-Fi radio (~40 mA draw). If the heater relay is active, it temporarily pauses the heater for `1.5 s` so the Intex 5 V rail recovers to full `5.0 V`, connects to Wi-Fi cleanly, and **automatically turns the heater back on `1.5 s` after connecting** (with a `25 s` offline failsafe so the spa never stays unheated during a home router outage). `WebConfig::begin()` no longer enables `WIFI_STA` prematurely before Stage B's local-bus heater check on recovery boots (and also starts cleanly in offline fallback mode), while `/scan` feeds the watchdog and frees scan memory via `WiFi.scanDelete()`.
- **80 MHz Wi-Fi Soft-Start → 160 MHz Bus Runtime:** Boots and performs `WiFi.mode(WIFI_STA)` RF calibration and initial Wi-Fi association at **`80 MHz`** (saving ~35–45 mA during the startup current peak), then steps up to **`160 MHz`** once connected for zero-drop 100 kHz bus ISR decoding.
- **Low-Current Runtime Profile & NVS-Persisted TX Power:** Enables **`WIFI_PS_MIN_MODEM`** (~75 mA average current reduction compared to `setSleep(false)`) and defaults Wi-Fi transmit power to **`8.5 dBm`**. Any TX power level (`8.5`, `11`, `13`, `15`, `17`, `19.5 dBm`) selected in Home Assistant (`select.intex_purespa_wifi_tx_power`) is persisted in NVS flash (`Preferences`) across full power cycles.

### 4. Staged Wi-Fi Recovery & Silent Boot
- **Staged Wi-Fi Recovery (No AP Boot-Loops):**
  - Starts in `WIFI_STA` when an SSID is configured (checks actual non-empty SSID string, not just pointer existence).
  - Keeps `pureSpaIO.loop()` running during temporary Wi-Fi drops.
  - Performs gentle `WiFi.reconnect()` attempts every **30 s**.
  - Starts the fallback configuration SoftAP (`Intex_PureSpa`) only after **3 minutes** continuously offline (and disables it automatically once Wi-Fi reconnects).
  - Performs a clean self-healing `ESP.restart()` after **10 minutes** offline — **never** while a user is connected to the fallback AP (`softAPgetStationNum() > 0`).
- **Silent Boot & Reconnect (No Night-Time Beeps):** Upstream v1.4.0 automatically powered on an OFF spa on every boot and MQTT broker restart just to read the setpoint (3 loud beeps on the terrace). This fork **never** powers on a turned-off spa automatically: target temperature is restored from RTC RAM on warm reboots, and on cold boots while OFF it waits silently until the spa is turned on normally.

### 5. MQTT Hardening & Home Assistant Enhancements
- **Bidirectional Physical Pool-Panel Setpoint Sync:** Detects target temperature changes made directly via the physical `▲`/`▼` buttons on the pool display (7-segment blink detection across all temperature deltas including `±1 °C` and `target == water`, plus immediate buzzer/blink water-temperature suppression so `water/tempAct` never briefly jumps to the blinking target temperature). Publishes the new target temperature to `Intex_PureSpa/water/tempSet` with `control/source = Pool-Panel` as soon as the blink cycle finishes.
- **Full Retained MQTT State & `UNDEF::BOOL` Guards:** Publishes `power`, `filter`, `bubble`, `heater`, `heaterMode`, `climateMode`, `climateAction`, and `error` with `retain=true` (alongside `water/tempAct` and `water/tempSet`), and explicitly checks `!= PureSpaIO::UNDEF::BOOL` before boolean casts so uninitialized `255` states never report `"on"`.
- **Eliminated 500 ms Publish Storm & `±1 °C` Flapping:** Removed `force = true` on unchanged `WATER_ACT` and `ERROR` publishes (replaced by change-detection + 60 s heartbeat), retained `WATER_ACT` (`Intex_PureSpa/water/tempAct`) so Home Assistant restarts never show `unknown °C` while online, and added a **5-second stability debounce** for `±1 °C` water temperature boundary fluctuations (fixing the upstream `if (diffTemp <= 1) allowPublish = true;` bypass).
- **Hardened 7-Segment Display Number Decoder (`display2Num`):** Cleanly handles both leading `'0'` and leading blank `' '` digits (e.g. `"027C"`, `" 27C"`, `" 81F"`, `"104F"`), preventing `-99` (`UNDEF`) temperature readouts.
- **Clean MQTT Reconnects:** Clears `MQTTClient::publications` deduplication cache and resets `MQTTPublisher` water-temp state on broker reconnect so Home Assistant immediately receives fresh values; reduced MQTT socket timeout from 15 s to 5 s.
- **Fixed `Heater` Switch in Green `standby` Mode:** Added a `value_template` (`{{ 'on' if value in ['on', 'standby'] else 'off' }}`) so `switch.intex_purespa_heater` stays `on` instead of becoming `unknown` when the spa reaches target temperature and enters standby.
- **Auto-Cleanup of Unused `Controller Temp`:** Only publishes HA discovery for `Controller Temp` if a valid NTC thermistor is actually attached to `GPIO34`; otherwise clears the permanently `unavailable` ghost entity automatically.
- **New Home Assistant MQTT Auto-Discovery Entities:**
  - `sensor.intex_purespa_heater_mode` (`heating` / `standby` / `off`)
  - `sensor.intex_purespa_letzte_bedienung` (`Home Assistant` / `Pool-Panel` / `Web-UI` — detects manual physical button presses at the spa!)
  - `select.intex_purespa_wifi_tx_power` (`8.5 dBm` – `19.5 dBm`, live adjustable)
  - `button.intex_purespa_heiz_timer_reset_72h` (cleanly cycles heater OFF → 2.5 s → ON to reset the Intex 72-hour `END` auto-shutoff timer)
  - `sensor.intex_purespa_reset_crash_status` (e.g. `SW_RESTART (boots:1, crash:NONE)` or `BROWNOUT (boots:3, crash:BROWNOUT@1840s)`)
  - `sensor.intex_purespa_uptime` (seconds, `device_class: duration`)
  - `sensor.intex_purespa_free_heap` (bytes)
  - `sensor.intex_purespa_wifi_ap_bssid` (connected AP MAC & Wi-Fi channel)
  - `sensor.intex_purespa_boot_count` & `sensor.intex_purespa_wifi_reconnects`

### 6. JSON Diagnostics API & Local Web Remote Control
- **`GET http://<esp-ip>/status`:** Lightweight JSON endpoint exposing firmware version, CPU MHz, TX power, Wi-Fi sleep mode, uptime, boot count, reset/crash reason, free/min heap, RSSI, BSSID, channel, MQTT/bus state, temperatures, live 4-character raw 7-segment display readout (`raw_display`, `display_blinking`), heater mode, control source, and total/dropped bus frames.
- **Local Web Remote (`http://<esp-ip>/`):** Live spa readout and one-click control buttons (`Power`, `Heizung`, `Filter`, `Blubber`, `-1 °C`, `+1 °C`, `72h-Reset`) via non-blocking `POST /cmd?action=...` — works even if Home Assistant is offline or restarting.

---

## Backwards Compatibility (Zero-Reconfig Upgrade from v1.4.0)

- **`ConfigurationFile.h` is 100% unchanged** (`MAGIC = 0x49535041`, `VERSION = 2`, `EEPROM_SIZE = 2048`). Existing Wi-Fi credentials, MQTT settings, spa model selection, and discovery prefix survive OTA upgrades untouched.
- **All GPIO pins and existing `Intex_PureSpa/...` MQTT topics are unchanged.**

---

## Wiring (ESP32)

| Signal | ESP32 GPIO | Direction |
|---|---|---|
| `CLOCK` | `GPIO18` | Input (100 kHz rising-edge IRAM ISR) |
| `DATA` | `GPIO19` | Input / Open-Drain Output (3 µs low ACK pulse) |
| `LATCH` / `HOLD` | `GPIO23` | Input |
| `NTC` (optional) | `GPIO34` | ADC1 Input (optional on-board thermistor) |

### Power Supply Recommendations
- Power via `5V` / `VIN` and `GND`.
- While v1.5.0 significantly lowers both average current (`WIFI_PS_MIN_MODEM`) and peak TX spikes (`11 dBm` + `160 MHz`), some Intex control boards still have limited 5 V current reserve during multi-hour continuous heating.
- If `sensor.intex_purespa_reset_crash_status` still reports `BROWNOUT` resets on your unit:
  1. Try lowering `WiFi TX Power` to `8.5 dBm` (if RSSI is better than `-70 dBm`).
  2. Add a **470–1000 µF Low-ESR electrolytic capacitor** (+ `100 nF` ceramic) across `5V` and `GND` close to the ESP32.
  3. For 100% decoupling, power the ESP32 from a dedicated 5 V USB supply (≥ 1 A): **disconnect/insulate the red 5 V wire** from the Intex bus, and **keep `GND` connected** as common signal ground.

---

## Upgrading via OTA (from v1.4.0 or earlier)

Prebuilt app-only binary: [`bin/esp32-intexsbh20.bin`](bin/esp32-intexsbh20.bin) (`~1.07 MB`, compatible with standard 4 MB ESP32 `default` dual-OTA partition table).

1. **Recommended before upgrading from stock v1.4.0:** Temporarily pause the spa heater (standby mode) so the 5 V rail has maximum headroom during flash writes.
2. Upload `bin/esp32-intexsbh20.bin` via the web UI at `http://<esp-ip>/update`, or throttle the upload gently via `curl` (especially helpful when upgrading from stock v1.4.0 whose non-IRAM ISR is still running during the upload):
   ```bash
   curl --limit-rate 24K -H "Expect:" -F "update=@bin/esp32-intexsbh20.bin;type=application/octet-stream" http://<esp-ip>/update
   ```
3. Wait ~10 seconds for the ESP32 to reboot and verify `http://<esp-ip>/status`.

---

## Building from Source (`arduino-cli`)

Tested with **Arduino-ESP32 `3.3.7`** (ESP-IDF 5.5.2) and **`PubSubClient` `2.8.0`**:

```bash
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.7 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install PubSubClient

arduino-cli compile \
  --fqbn "esp32:esp32:esp32:PartitionScheme=default,FlashFreq=80,FlashMode=dio,FlashSize=4M" \
  src/esp32-intexsbh20
```

---

## License

This project is a derivative work based on [caspercze/esp32-intexsbh20](https://github.com/caspercze/esp32-intexsbh20) and [jnsbyr/esp8266-intexsbh20](https://github.com/jnsbyr/esp8266-intexsbh20), incorporating protocol decoding from DIYSCIP by Geoffroy Hubert (CC BY-NC-SA 4.0) and Apache-2.0 licensed components.

For non-commercial use, this repository follows the terms of **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 (CC BY-NC-SA 4.0)**. Please refer to `LICENSE.txt` and the original upstream repositories for full license details.
