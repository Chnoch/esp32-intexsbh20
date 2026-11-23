/*
 * project:  Intex PureSpa WiFi Controller
 *
 * file:     esp32-intexsbh20.ino
 *
 * encoding: UTF-8
 * created:  23rd March 2021
 *
 * Copyright (C) 2021 Jens B.
 *
 * Enhanced and maintained by Petr Kašpar (27 March 2026).
 * https://github.com/caspercze
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware:
 * ESP32 firmware; SB-H20 and SJB-HS panel models.
 * Target: ESP32 (Arduino-ESP32; GPIO numbers refer to the chip, not the silkscreen).
 * GPIO pins (see common.h, namespace PIN):
 *   18  CLOCK  — spa panel bus clock (digital input, interrupt on rising edge)
 *   19  DATA   — spa panel bus data line (input; open-drain when driving)
 *   23  LATCH  — spa panel bus latch / frame sync (digital input)
 *   34  NTC    — ADC1, on-board NTC thermistor for controller temperature
 */

#include "common.h"
#include "ConfigurationFile.h"
#include "MQTTClient.h"
#include "MQTTPublisher.h"
#include "NTCThermometer.h"
#include "OTAUpdate.h"
#include "PureSpaIO.h"
#include "WebConfig.h"
#include <Preferences.h>
#include <esp_system.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <stdexcept>

ConfigurationFile config;
NTCThermometer thermometer;
OTAUpdate otaUpdate;
PureSpaIO pureSpaIO;
WebConfig webConfig;

MQTTClient mqttClient;
MQTTPublisher mqttPublisher(mqttClient, pureSpaIO, thermometer);
String username;
String password;
String mqttDeviceName;
String discoveryPrefix;

unsigned long disconnectTime = 0;
static unsigned long lastWifiReconnectAttemptMs = 0;

bool g_wdtInitialized = false;
RTC_NOINIT_ATTR RtcDiagState g_rtcDiag;
uint32_t g_wifiReconnectCount = 0;

struct RtcTxPowerState
{
  uint32_t magic;
  uint8_t  idx;
};
static const uint32_t RTC_TX_POWER_MAGIC = 0x54585057UL; // "TXPW"
RTC_NOINIT_ATTR RtcTxPowerState g_rtcTxPower;

struct RtcRecoveryState
{
  uint32_t magic;
  uint8_t  wifiStartAttempts;
  uint8_t  heaterPausedForBoot;
};
static const uint32_t RTC_RECOVERY_MAGIC = 0x53504152UL; // "SPAR"
RTC_NOINIT_ATTR RtcRecoveryState g_rtcRecovery;
static unsigned long g_wifiConnectedSinceMs = 0;

static const wifi_power_t TX_POWER_VALUES[6] = {
  WIFI_POWER_8_5dBm,
  WIFI_POWER_11dBm,
  WIFI_POWER_13dBm,
  WIFI_POWER_15dBm,
  WIFI_POWER_17dBm,
  WIFI_POWER_19_5dBm
};
static const char* const TX_POWER_LABELS[6] = {
  "8.5 dBm",
  "11 dBm",
  "13 dBm",
  "15 dBm",
  "17 dBm",
  "19.5 dBm"
};

static uint8_t ensureTxPowerIdx()
{
  if (g_rtcTxPower.magic != RTC_TX_POWER_MAGIC || g_rtcTxPower.idx >= 6)
  {
    g_rtcTxPower.magic = RTC_TX_POWER_MAGIC;
    g_rtcTxPower.idx = 0; // Default: 8.5 dBm (lowest brownout risk)
  }
  return g_rtcTxPower.idx;
}

static void initTxPowerFromNvs()
{
  Preferences prefs;
  uint8_t savedIdx = 0; // Default: 8.5 dBm
  if (prefs.begin("intex_cfg", true))
  {
    uint8_t v = prefs.getUChar("tx_idx", 0);
    if (v < 6)
    {
      savedIdx = v;
    }
    prefs.end();
  }
  g_rtcTxPower.magic = RTC_TX_POWER_MAGIC;
  g_rtcTxPower.idx = savedIdx;
}

const char* getTxPowerOptionLabel()
{
  return TX_POWER_LABELS[ensureTxPowerIdx()];
}

void applyWifiPowerProfile()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.setTxPower(TX_POWER_VALUES[ensureTxPowerIdx()]);
}

bool setTxPowerByLabel(const char* label)
{
  if (!label) return false;
  for (uint8_t i = 0; i < 6; ++i)
  {
    if (strcmp(label, TX_POWER_LABELS[i]) == 0)
    {
      const bool changed = (g_rtcTxPower.magic != RTC_TX_POWER_MAGIC || g_rtcTxPower.idx != i);
      g_rtcTxPower.magic = RTC_TX_POWER_MAGIC;
      g_rtcTxPower.idx = i;
      if (changed)
      {
        pureSpaIO.pauseBusISR();
        Preferences prefs;
        if (prefs.begin("intex_cfg", false))
        {
          prefs.putUChar("tx_idx", i);
          prefs.end();
        }
        pureSpaIO.resumeBusISR();
      }
      applyWifiPowerProfile();
      return true;
    }
  }
  return false;
}

static char g_controlSource[24] = "Start";
static unsigned long g_lastRemoteCmdMs = 0;
static char g_pendingWebCmd[16] = "";
static bool g_pendingReset72h = false;
static uint8_t g_prevPanelStateBits = 0xFF;

void markControlSource(const char* source)
{
  if (!source) return;
  strncpy(g_controlSource, source, sizeof(g_controlSource) - 1);
  g_controlSource[sizeof(g_controlSource) - 1] = '\0';
  if (strcmp(source, "Home Assistant") == 0 || strcmp(source, "Web-UI") == 0)
  {
    g_lastRemoteCmdMs = millis();
  }
}

const char* getControlSource()
{
  return g_controlSource;
}

bool wasRecentRemoteCommand(unsigned long nowMs, unsigned long windowMs)
{
  return (g_lastRemoteCmdMs != 0) && (timeDiff(nowMs, g_lastRemoteCmdMs) <= windowMs);
}

void requestSpaWebCommand(const char* action)
{
  if (!action) return;
  strncpy(g_pendingWebCmd, action, sizeof(g_pendingWebCmd) - 1);
  g_pendingWebCmd[sizeof(g_pendingWebCmd) - 1] = '\0';
}

const char* resetReasonToString(int32_t reason)
{
  switch (reason)
  {
    case 0:                 return "NONE";
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SW_RESTART";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

static bool isAbnormalResetReason(int32_t r)
{
  return (r == ESP_RST_PANIC)
      || (r == ESP_RST_INT_WDT)
      || (r == ESP_RST_TASK_WDT)
      || (r == ESP_RST_WDT)
      || (r == ESP_RST_BROWNOUT);
}

LANG language = LANG::CODE;
bool initialized = false;
bool otaStatusInitialized = false;
static bool prevMqttConnected = false;

// Debounce for temperature commands (avoid repeated setpoint changes).
static const unsigned long WATER_SET_DEBOUNCE_MS = 1500;
int pendingWaterSetTempC = PureSpaIO::UNDEF::INT;
unsigned long pendingWaterSetTempAtMs = 0;

// Boot-only one-shot sync of tempSet from panel UI.
enum BootTempSetSyncState
{
  BOOT_TEMPSET_IDLE = 0,
  BOOT_TEMPSET_WAIT_POWER_ON,
  BOOT_TEMPSET_WAIT_READ,
  BOOT_TEMPSET_WAIT_POWER_OFF,
  BOOT_TEMPSET_DONE
};

BootTempSetSyncState bootTempSetSyncState = BOOT_TEMPSET_IDLE;
bool bootTempSetRestorePowerOff = false;
unsigned long bootTempSetStepAtMs = 0;
uint8_t bootTempSetPowerOffRetries = 0;
unsigned long bootSpaOnlineSinceMs = 0;
static const unsigned long BOOT_TEMPSET_START_AFTER_ONLINE_MS = 1200;
bool bootSwitchSnapshotPublished = false;

// After manual/power-on: one arrow click if tempSet still unknown (boot discovery already finished).
static const unsigned long POWER_ON_TEMP_PROBE_DELAY_MS = 800;
static bool prevSpaPowerOnSample = false;
static bool prevSpaPowerOnValid = false;
static unsigned long powerOnTempProbeScheduledAtMs = 0;

static String readConfigOrDefault(const char* key, const char* fallback = "")
{
  if (config.exists(key))
  {
    return String(config.get(key));
  }
  return String(fallback ? fallback : "");
}

static bool hasConfiguredWifiSsid()
{
  const char* ssid = config.get(CONFIG_TAG::WIFI_SSID);
  return (ssid != nullptr) && (ssid[0] != '\0');
}

void setup()
{
  // Stage A: Disarm hair-trigger 2.43 V RTC brownout interrupt so microsecond Wi-Fi RF pulses
  // behind the NodeMCU AMS1117-3.3 dropout do not trigger false esp_restart_noos() resets.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Soft-start at 80 MHz during brownout backoff, Wi-Fi PHY RF-calibration & initial connect
  // to save ~35-45 mA right on top of the critical WiFi.mode() current spike.
  // CPU steps up to 160 MHz in loop() once Wi-Fi is connected and before the 100 kHz bus ISR starts.
  setCpuFrequencyMhz(80);
  Serial.begin(74880);
  delay(20);

  const esp_reset_reason_t rr = esp_reset_reason();
  const int32_t rrCode = static_cast<int32_t>(rr);
  Serial.printf("ESP reset reason %d (%s)\n", (int)rrCode, resetReasonToString(rrCode));

  if (g_rtcRecovery.magic != RTC_RECOVERY_MAGIC
      || g_rtcRecovery.wifiStartAttempts > 100
      || g_rtcRecovery.heaterPausedForBoot > 1)
  {
    g_rtcRecovery.magic = RTC_RECOVERY_MAGIC;
    g_rtcRecovery.wifiStartAttempts = 0;
    g_rtcRecovery.heaterPausedForBoot = 0;
  }

  const bool rtcSurvivedDeepSag =
    (g_rtcDiag.magic == RTC_DIAG_MAGIC)
    && (rr == ESP_RST_POWERON)
    && (g_rtcRecovery.wifiStartAttempts > 0 || g_rtcDiag.currentUptimeSec > 0);

  if (g_rtcDiag.magic != RTC_DIAG_MAGIC || (rr == ESP_RST_POWERON && !rtcSurvivedDeepSag))
  {
    g_rtcDiag.magic = RTC_DIAG_MAGIC;
    g_rtcDiag.bootCount = 1;
    g_rtcDiag.lastResetReason = rrCode;
    g_rtcDiag.lastCrashReason = isAbnormalResetReason(rrCode) ? rrCode : 0;
    g_rtcDiag.lastCrashUptimeSec = 0;
    g_rtcDiag.currentUptimeSec = 0;
    g_rtcDiag.savedSetTempC = PureSpaIO::UNDEF::INT;
    g_rtcRecovery.wifiStartAttempts = 0;
    g_rtcRecovery.heaterPausedForBoot = 0;
  }
  else
  {
    g_rtcDiag.bootCount++;
    g_rtcDiag.lastResetReason = rtcSurvivedDeepSag ? (int32_t)ESP_RST_BROWNOUT : rrCode;
    if (isAbnormalResetReason(rrCode) || rtcSurvivedDeepSag)
    {
      g_rtcDiag.lastCrashReason = rtcSurvivedDeepSag ? (int32_t)ESP_RST_BROWNOUT : rrCode;
      g_rtcDiag.lastCrashUptimeSec = g_rtcDiag.currentUptimeSec;
    }
    g_rtcDiag.currentUptimeSec = 0;
    if (g_rtcDiag.savedSetTempC >= PureSpaIO::WATER_TEMP::SET_MIN
        && g_rtcDiag.savedSetTempC <= PureSpaIO::WATER_TEMP::SET_MAX)
    {
      pureSpaIO.setAuthoritativeDesiredCelsius(g_rtcDiag.savedSetTempC);
    }
  }

  if (rr == ESP_RST_BROWNOUT || rtcSurvivedDeepSag || g_rtcRecovery.wifiStartAttempts >= 1)
  {
    // Give the 5V supply rail 3-5 seconds at low-power 80 MHz idle (Wi-Fi OFF) to cool & recover cleanly
    const unsigned long backoffMs = (g_rtcDiag.bootCount >= 5) ? 5000UL : 3000UL;
    delay(backoffMs);
  }

  initTxPowerFromNvs();

  // Configure 30-second hardware task watchdog for Core 0 (Wi-Fi/LwIP IDLE0) AND Core 1 (loopTask)
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 30000,
    .idle_core_mask = (1U << 0),
    .trigger_panic = true
  };
  if (esp_task_wdt_reconfigure(&wdtConfig) != ESP_OK)
  {
    esp_task_wdt_init(&wdtConfig);
  }
  esp_err_t wdtAddErr = esp_task_wdt_add(NULL);
  g_wdtInitialized = (wdtAddErr == ESP_OK || wdtAddErr == ESP_ERR_INVALID_ARG);

  config.load();

  Serial.printf("%s MQTT WiFi Controller %s (%u MHz)\n", pureSpaIO.getModelName(), CONFIG::WIFI_VERSION, (unsigned int)getCpuFrequencyMhz());
  Serial.printf("build with Arduino Core for ESP32 %s\n", ESP.getSdkVersion());

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  webConfig.begin();

  bool ready = false;
  if (config.load())
  {
    try
    {
      String forceSleep = readConfigOrDefault(CONFIG_TAG::FORCE_WIFI_SLEEP_KEY, "no");
      pureSpaIO.setForceWifiSleep(forceSleep != "no");

      String selectedModel = readConfigOrDefault(CONFIG_TAG::SELECTED_MODEL_KEY, CONFIG::DEFAULT_SELECTED_MODEL);
      pureSpaIO.setModelFromString(selectedModel.c_str());

      if (hasConfiguredWifiSsid())
      {
        // Stage B: Automatic local-bus Heater-Pause before Wi-Fi start on any brownout / recovery boot.
        // While Wi-Fi is OFF (~40 mA draw), the ESP32 is 100% stable and can pause the heater relay locally
        // over GPIO 18/19/23 so the Intex 5V rail jumps back to 5.0V for Wi-Fi startup, then auto-resume in loop().
        const bool needHeaterPauseCheck =
          (rr == ESP_RST_BROWNOUT)
          || rtcSurvivedDeepSag
          || (g_rtcRecovery.wifiStartAttempts >= 1)
          || (g_rtcDiag.bootCount > 1 && rr != ESP_RST_SW);

        if (needHeaterPauseCheck)
        {
          setCpuFrequencyMhz(160);
          pureSpaIO.setup(language);
          for (int i = 0; i < 40; ++i)
          {
            delay(20);
            pureSpaIO.loop();
            feedWatchdog();
            if (pureSpaIO.isOnline() && pureSpaIO.isHeaterOn() != PureSpaIO::UNDEF::BOOL)
            {
              break;
            }
          }

          if (pureSpaIO.isOnline() && pureSpaIO.isHeaterOn() == true)
          {
            Serial.println("Stage B recovery: pausing Intex heater locally before Wi-Fi start...");
            g_rtcRecovery.heaterPausedForBoot = 1;
            pureSpaIO.setHeaterOn(false);
            for (int i = 0; i < 15; ++i)
            {
              delay(100);
              pureSpaIO.loop();
              feedWatchdog();
            }
          }

          pureSpaIO.pauseBusISR();
          setCpuFrequencyMhz(80);
        }

        if (g_rtcRecovery.wifiStartAttempts < 100)
        {
          g_rtcRecovery.wifiStartAttempts++;
        }
        WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
        WiFi.mode(WIFI_STA);
        applyWifiPowerProfile();
        WiFi.begin(config.get(CONFIG_TAG::WIFI_SSID), config.get(CONFIG_TAG::WIFI_PASSPHRASE));
      }
      applyWifiPowerProfile();
      WiFi.setAutoReconnect(true);

      bool retainAll = readConfigOrDefault(CONFIG_TAG::MQTT_RETAIN, "no") != "no";
      mqttPublisher.setRetainAll(retainAll);

      mqttDeviceName = readConfigOrDefault(CONFIG_TAG::CUSTOM_MODEL_NAME_KEY, pureSpaIO.getModelName());
      if (!mqttDeviceName.length())
      {
        mqttDeviceName = pureSpaIO.getModelName();
      }

      discoveryPrefix = readConfigOrDefault(CONFIG_TAG::MQTT_DISCOVERY_PREFIX, CONFIG::DEFAULT_MQTT_DISCOVERY);
      String discoveryMode = readConfigOrDefault(CONFIG_TAG::MQTT_DISCOVERY_MODE, CONFIG::DEFAULT_MQTT_DISCOVERY_MODE);
      bool haDiscovery = (discoveryMode != "DIRECT");
      mqttClient.configureDiscovery(discoveryPrefix.c_str(), haDiscovery);

      mqttClient.addMetadata(
        MQTT_TOPIC::MODEL,
        pureSpaIO.getModel() == PureSpaIO::MODEL::SJBHS ? "SJB-HS" : "SB-H20 / SSP-H-20-1 / SB-B20"
      );
      mqttClient.addMetadata(MQTT_TOPIC::VERSION, CONFIG::WIFI_VERSION);
      mqttClient.addMetadata(MQTT_TOPIC::IP, WiFi.localIP().toString().c_str());

      mqttClient.addSubscriber(MQTT_TOPIC::CMD_BUBBLE, [](bool b) -> void {
        markControlSource("Home Assistant");
        pureSpaIO.setBubbleOn(b);
      });

      mqttClient.addSubscriber(MQTT_TOPIC::CMD_FILTER, [](bool b) -> void {
        markControlSource("Home Assistant");
        pureSpaIO.setFilterOn(b);
      });

      mqttClient.addSubscriber(MQTT_TOPIC::CMD_HEATER, [](bool b) -> void {
        markControlSource("Home Assistant");
        pureSpaIO.markTempDisplayDisturbance();
        if (b)
        {
          // HA climate "heat" should turn the whole system on (if needed)
          // and then enable heating.
          if (!pureSpaIO.isPowerOn())
          {
            pureSpaIO.setPowerOn(true);

            // Wait a short moment until the power state updates.
            unsigned long start = millis();
            while (!pureSpaIO.isPowerOn() && timeDiff(millis(), start) < 3000)
            {
              delay(100);
              feedWatchdog();
            }
          }

          pureSpaIO.setHeaterOn(true);
        }
        else
        {
          // HA climate "off" should disable heating.
          pureSpaIO.setHeaterOn(false);
        }
      });

      mqttClient.addSubscriber(MQTT_TOPIC::CMD_POWER, [](bool b) -> void {
        markControlSource("Home Assistant");
        pureSpaIO.markTempDisplayDisturbance();
        pureSpaIO.setPowerOn(b);
        delay(200);
        feedWatchdog();

        mqttClient.publish(MQTT_TOPIC::POWER, pureSpaIO.isPowerOn() ? "on" : "off", true, true);
      });

      mqttClient.addSubscriber(MQTT_TOPIC::CMD_WATER, [](int i) -> void {
        if (i < PureSpaIO::WATER_TEMP::SET_MIN || i > PureSpaIO::WATER_TEMP::SET_MAX)
        {
          return;
        }

        markControlSource("Home Assistant");
        pureSpaIO.markTempDisplayDisturbance();
        pureSpaIO.setAuthoritativeDesiredCelsius(i);

        // Debounce: only apply after there was no new command for a moment.
        pendingWaterSetTempC = i;
        pendingWaterSetTempAtMs = millis();

        // Stop the startup "sync displayed setpoint" logic while the user is controlling the setpoint.
      });

      mqttClient.addStringSubscriber(MQTT_TOPIC::CMD_TX_POWER, [](const char* val) -> void {
        if (setTxPowerByLabel(val))
        {
          mqttClient.publish(MQTT_TOPIC::TX_POWER, String(getTxPowerOptionLabel()), true, true);
        }
      });

      mqttClient.addStringSubscriber(MQTT_TOPIC::CMD_RESET_72H, [](const char* val) -> void {
        (void)val;
        markControlSource("Home Assistant");
        g_pendingReset72h = true;
      });

      if (pureSpaIO.getModel() == PureSpaIO::MODEL::SJBHS)
      {
        mqttClient.addSubscriber(MQTT_TOPIC::CMD_DISINFECTION, [](int i) -> void {
          pureSpaIO.setDisinfectionTime(i);
        });

        mqttClient.addSubscriber(MQTT_TOPIC::CMD_JET, [](bool b) -> void {
          pureSpaIO.setJetOn(b);
        });

        mqttClient.addMetadata(MQTT_TOPIC::DISINFECTION, "enabled");
        mqttClient.addMetadata(MQTT_TOPIC::JET, "enabled");
      }

      if (config.exists(CONFIG_TAG::MQTT_ERROR_LANG))
      {
        String lang = config.get(CONFIG_TAG::MQTT_ERROR_LANG);
        if (lang == "EN")
        {
          language = LANG::EN;
        }
        else if (lang == "DE")
        {
          language = LANG::DE;
        }
        else if (lang == "CZ")
        {
          language = LANG::CZ;
        }
        else
        {
          language = LANG::CODE;
        }
      }

      if (config.exists(CONFIG_TAG::MQTT_USER))
      {
        username = config.get(CONFIG_TAG::MQTT_USER);
        password = config.get(CONFIG_TAG::MQTT_PASSWORD);
      }

      uint16 mqttPort = config.exists(CONFIG_TAG::MQTT_PORT) ? atoi(config.get(CONFIG_TAG::MQTT_PORT)) : 1883;

      mqttClient.setup(
        config.get(CONFIG_TAG::MQTT_SERVER),
        mqttPort,
        username.c_str(),
        password.c_str(),
        mqttDeviceName.c_str(),
        MQTT_TOPIC::STATE,
        "offline"
      );

      thermometer.setup(22000, 3.30f, 320.f / 100.f);
      ready = true;
    }
    catch (const std::runtime_error& re)
    {
      Serial.println(re.what());
    }
  }

  if (!ready)
  {
    Serial.println("Config not ready, web config is available.");
    return;
  }
}

void loop()
{
  webConfig.loop();
  feedWatchdog();

  wl_status_t wifiStatus = WiFi.status();
  unsigned long now = millis();
  g_rtcDiag.currentUptimeSec = now / 1000UL;

  if (wifiStatus == WL_CONNECTED)
  {
    if (disconnectTime != 0)
    {
      applyWifiPowerProfile();
    }
    disconnectTime = 0;
    lastWifiReconnectAttemptMs = 0;

    if (!initialized)
    {
      // Wi-Fi is connected and running in low-TX / modem-sleep mode; now step up to 160 MHz for 100 kHz bus ISR.
      setCpuFrequencyMhz(160);
      applyWifiPowerProfile();
      mqttClient.addMetadata(MQTT_TOPIC::IP, WiFi.localIP().toString().c_str());
      pureSpaIO.setup(language);

      initialized = true;
      g_wifiConnectedSinceMs = now;
      otaStatusInitialized = false;
      bootTempSetSyncState = BOOT_TEMPSET_IDLE;
      bootTempSetRestorePowerOff = false;
      bootTempSetStepAtMs = 0;
      bootTempSetPowerOffRetries = 0;
      bootSpaOnlineSinceMs = 0;
      bootSwitchSnapshotPublished = false;
      prevSpaPowerOnValid = false;
      prevSpaPowerOnSample = false;
      powerOnTempProbeScheduledAtMs = 0;
    }
    else
    {
      mqttClient.loop();

      if (mqttClient.isConnected() && !prevMqttConnected)
      {
        mqttPublisher.resetPublishedState();
        mqttPublisher.publishWifiDiagnosticsNow();
      }
      prevMqttConnected = mqttClient.isConnected();

      if (mqttClient.isConnected() && !otaStatusInitialized)
      {
        // Make OTA status immediately available in HA after first connect/restart.
        mqttClient.publish(MQTT_TOPIC::OTA, "idle", true, true);
        otaStatusInitialized = true;
      }
      else if (!mqttClient.isConnected())
      {
        otaStatusInitialized = false;
        bootSwitchSnapshotPublished = false;
        prevSpaPowerOnValid = false;
        prevSpaPowerOnSample = false;
        powerOnTempProbeScheduledAtMs = 0;
      }

      if (pureSpaIO.isOnline())
      {
        if (bootSpaOnlineSinceMs == 0)
        {
          bootSpaOnlineSinceMs = now;
        }
      }
      else
      {
        bootSpaOnlineSinceMs = 0;
      }

      pureSpaIO.loop();

      // Stage B completion: once Wi-Fi is stably connected, automatically re-enable heater if we paused it for boot.
      if (pureSpaIO.isOnline() && g_wifiConnectedSinceMs != 0)
      {
        if (g_rtcRecovery.heaterPausedForBoot == 1 && timeDiff(now, g_wifiConnectedSinceMs) >= 1500UL)
        {
          g_rtcRecovery.heaterPausedForBoot = 0;
          if (pureSpaIO.isPowerOn() == true && pureSpaIO.isHeaterOn() == false)
          {
            Serial.println("Stage B recovery: Wi-Fi connected, automatically resuming Intex heater...");
            g_lastRemoteCmdMs = millis();
            pureSpaIO.markTempDisplayDisturbance();
            pureSpaIO.setHeaterOn(true);
            bootSwitchSnapshotPublished = false;
          }
        }
        if (g_rtcRecovery.wifiStartAttempts != 0 && timeDiff(now, g_wifiConnectedSinceMs) >= 10000UL)
        {
          g_rtcRecovery.wifiStartAttempts = 0;
        }
      }

      // Detect manual button presses directly on the physical Intex pool panel
      if (pureSpaIO.isOnline() && bootSwitchSnapshotPublished)
      {
        const uint8 pwrS = pureSpaIO.isPowerOn();
        const uint8 htrS = pureSpaIO.isHeaterOn();
        const uint8 fltS = pureSpaIO.isFilterOn();
        const uint8 bblS = pureSpaIO.isBubbleOn();
        if (pwrS != PureSpaIO::UNDEF::BOOL && htrS != PureSpaIO::UNDEF::BOOL
            && fltS != PureSpaIO::UNDEF::BOOL && bblS != PureSpaIO::UNDEF::BOOL)
        {
          const uint8 bits = ((pwrS == true) ? 1 : 0)
                           | ((htrS == true) ? 2 : 0)
                           | ((fltS == true) ? 4 : 0)
                           | ((bblS == true) ? 8 : 0);
          if (g_prevPanelStateBits != 0xFF && bits != g_prevPanelStateBits)
          {
            if (!wasRecentRemoteCommand(now, 6000UL))
            {
              markControlSource("Pool-Panel");
            }
          }
          g_prevPanelStateBits = bits;
        }
      }

      // Execute queued Web-UI local remote command (non-blocking HTTP response)
      if (g_pendingWebCmd[0] != '\0' && pureSpaIO.isOnline())
      {
        char act[16];
        strncpy(act, g_pendingWebCmd, sizeof(act) - 1);
        act[sizeof(act) - 1] = '\0';
        g_pendingWebCmd[0] = '\0';

        markControlSource("Web-UI");
        if (strcmp(act, "power") == 0)
        {
          pureSpaIO.markTempDisplayDisturbance();
          pureSpaIO.setPowerOn(pureSpaIO.isPowerOn() != true);
        }
        else if (strcmp(act, "heater") == 0)
        {
          pureSpaIO.markTempDisplayDisturbance();
          bool targetOn = (pureSpaIO.isHeaterOn() != true);
          if (targetOn && pureSpaIO.isPowerOn() != true)
          {
            pureSpaIO.setPowerOn(true);
            delay(500);
            feedWatchdog();
          }
          pureSpaIO.setHeaterOn(targetOn);
        }
        else if (strcmp(act, "filter") == 0)
        {
          pureSpaIO.setFilterOn(pureSpaIO.isFilterOn() != true);
        }
        else if (strcmp(act, "bubble") == 0)
        {
          pureSpaIO.setBubbleOn(pureSpaIO.isBubbleOn() != true);
        }
        else if (strcmp(act, "temp_up") == 0 || strcmp(act, "temp_down") == 0)
        {
          int curT = pureSpaIO.getAuthoritativeDesiredWaterTempCelsius();
          if (curT == PureSpaIO::UNDEF::INT) curT = pureSpaIO.getActWaterTempCelsius();
          if (curT != PureSpaIO::UNDEF::INT)
          {
            int nextT = curT + (strcmp(act, "temp_up") == 0 ? 1 : -1);
            if (nextT >= PureSpaIO::WATER_TEMP::SET_MIN && nextT <= PureSpaIO::WATER_TEMP::SET_MAX)
            {
              pureSpaIO.markTempDisplayDisturbance();
              pureSpaIO.setAuthoritativeDesiredCelsius(nextT);
              pendingWaterSetTempC = nextT;
              pendingWaterSetTempAtMs = now;
            }
          }
        }
        else if (strcmp(act, "reset72h") == 0)
        {
          g_pendingReset72h = true;
        }
      }

      // Execute 72-hour heater auto-shutoff reset (Heater OFF -> 2.5s -> Heater ON)
      if (g_pendingReset72h && pureSpaIO.isOnline())
      {
        g_pendingReset72h = false;
        pureSpaIO.markTempDisplayDisturbance();
        if (pureSpaIO.isPowerOn() != true)
        {
          pureSpaIO.setPowerOn(true);
          for (int k = 0; k < 10; ++k) { delay(100); feedWatchdog(); }
        }
        if (pureSpaIO.isHeaterOn() == true)
        {
          pureSpaIO.setHeaterOn(false);
          for (int k = 0; k < 25; ++k) { delay(100); feedWatchdog(); }
        }
        pureSpaIO.setHeaterOn(true);
      }

      mqttPublisher.loop();

      // One-time forced switch snapshot for HA right after online/connect.
      if (mqttClient.isConnected() && pureSpaIO.isOnline() && !bootSwitchSnapshotPublished)
      {
        const uint8 bubbleState = pureSpaIO.isBubbleOn();
        const uint8 filterState = pureSpaIO.isFilterOn();
        const uint8 powerState = pureSpaIO.isPowerOn();
        const uint8 heaterState = pureSpaIO.isHeaterOn();

        if (bubbleState != PureSpaIO::UNDEF::BOOL
            && filterState != PureSpaIO::UNDEF::BOOL
            && powerState != PureSpaIO::UNDEF::BOOL)
        {
          const bool sjbhsReady =
            pureSpaIO.getModel() != PureSpaIO::MODEL::SJBHS
            || (pureSpaIO.isJetOn() != PureSpaIO::UNDEF::BOOL
                && pureSpaIO.getDisinfectionTime() != PureSpaIO::UNDEF::INT);

          if (sjbhsReady)
          {
            mqttClient.publish(MQTT_TOPIC::BUBBLE, bubbleState ? "on" : "off", true, true);
            mqttClient.publish(MQTT_TOPIC::FILTER, filterState ? "on" : "off", true, true);
            mqttClient.publish(MQTT_TOPIC::POWER, powerState ? "on" : "off", true, true);

            if (heaterState != PureSpaIO::UNDEF::BOOL)
            {
              mqttClient.publish(
                MQTT_TOPIC::HEATER,
                heaterState ? (pureSpaIO.isHeaterStandby() ? "standby" : "on") : "off",
                true,
                true
              );
            }

            if (pureSpaIO.getModel() == PureSpaIO::MODEL::SJBHS)
            {
              mqttClient.publish(
                MQTT_TOPIC::JET,
                pureSpaIO.isJetOn() ? "on" : "off",
                true,
                true
              );
              mqttClient.publish(
                MQTT_TOPIC::DISINFECTION,
                String(pureSpaIO.getDisinfectionTime()),
                true,
                true
              );
            }

            bootSwitchSnapshotPublished = true;
          }
        }
      }

      // Apply debounced water temperature setpoint
      if (pendingWaterSetTempC != PureSpaIO::UNDEF::INT
          && pendingWaterSetTempAtMs != 0
          && timeDiff(now, pendingWaterSetTempAtMs) >= WATER_SET_DEBOUNCE_MS)
      {
        // Only attempt when power state is explicitly ON.
        // Keep pending command while OFF/unknown so it is not lost.
        const uint8 powerState = pureSpaIO.isPowerOn();
        if (powerState == true)
        {
          int target = pendingWaterSetTempC;
          pureSpaIO.setDesiredWaterTempCelsius(target);
          delay(300);
          yield();

          int setTemp = pureSpaIO.getDesiredWaterTempCelsius();
          if (setTemp != PureSpaIO::UNDEF::INT)
          {
            pendingWaterSetTempC = PureSpaIO::UNDEF::INT;
            pendingWaterSetTempAtMs = 0;

            mqttClient.publish(MQTT_TOPIC::WATER_SET, String(setTemp), true, true);
            pureSpaIO.setAuthoritativeDesiredCelsius(setTemp);
            // Re-apply water suppression after button clicks settle.
            pureSpaIO.markTempDisplayDisturbance();
          }
          else
          {
            // Retry later if setpoint is not yet readable.
            pendingWaterSetTempAtMs = now;
          }
        }
      }

      // Boot-only tempSet discovery:
      // If HA does not know the setpoint yet, perform one safe sequence:
      // power on (if needed) -> request setpoint display -> read -> restore power state.
      const bool bootTempSetCanStart =
        mqttClient.isConnected()
        && pureSpaIO.isOnline()
        && bootSpaOnlineSinceMs != 0
        && timeDiff(now, bootSpaOnlineSinceMs) >= BOOT_TEMPSET_START_AFTER_ONLINE_MS
        && pendingWaterSetTempC == PureSpaIO::UNDEF::INT;

      const bool bootTempSetInProgress =
        bootTempSetSyncState != BOOT_TEMPSET_IDLE
        && bootTempSetSyncState != BOOT_TEMPSET_DONE;

      if (bootTempSetCanStart || bootTempSetInProgress)
      {
        // Only skip whole boot discovery when idle and tempSet already known (e.g. retained MQTT).
        // Do NOT jump to DONE mid-sequence — that would abort before power-off / panel read.
        if (bootTempSetSyncState == BOOT_TEMPSET_IDLE
            && pureSpaIO.getAuthoritativeDesiredWaterTempCelsius() != PureSpaIO::UNDEF::INT)
        {
          bootTempSetSyncState = BOOT_TEMPSET_DONE;
        }
        else if (bootTempSetSyncState != BOOT_TEMPSET_DONE)
        {
          switch (bootTempSetSyncState)
          {
            case BOOT_TEMPSET_IDLE:
            {
              const uint8 powerState = pureSpaIO.isPowerOn();
              if (powerState == PureSpaIO::UNDEF::BOOL)
              {
                break;
              }

              if (powerState == false)
              {
                // Never turn on a powered-off spa automatically on boot/reconnect.
                // Setpoint will be probed automatically via powerOnTempProbe once the spa is turned ON.
                bootTempSetRestorePowerOff = false;
                bootTempSetSyncState = BOOT_TEMPSET_DONE;
                break;
              }

              bootTempSetRestorePowerOff = false;
              bootTempSetStepAtMs = now;
              bootTempSetSyncState = BOOT_TEMPSET_WAIT_POWER_ON;
              break;
            }

            case BOOT_TEMPSET_WAIT_POWER_ON:
            {
              if (timeDiff(now, bootTempSetStepAtMs) < 1000)
              {
                break;
              }

              if (pureSpaIO.isPowerOn() != true)
              {
                if (bootTempSetRestorePowerOff)
                {
                  pureSpaIO.setPowerOn(false);
                }
                bootTempSetSyncState = BOOT_TEMPSET_DONE;
                break;
              }

              if (!pureSpaIO.requestSetTempDisplay())
              {
                if (bootTempSetRestorePowerOff)
                {
                  pureSpaIO.setPowerOn(false);
                }
                bootTempSetSyncState = BOOT_TEMPSET_DONE;
                break;
              }

              bootTempSetStepAtMs = now;
              bootTempSetSyncState = BOOT_TEMPSET_WAIT_READ;
              break;
            }

            case BOOT_TEMPSET_WAIT_READ:
            {
              int setTemp = pureSpaIO.getDesiredWaterTempCelsius();
              if (setTemp != PureSpaIO::UNDEF::INT)
              {
                pureSpaIO.setAuthoritativeDesiredCelsius(setTemp);
                mqttClient.publish(MQTT_TOPIC::WATER_SET, String(setTemp), true, true);

                if (bootTempSetRestorePowerOff)
                {
                  pureSpaIO.setPowerOn(false);
                  mqttClient.publish(MQTT_TOPIC::POWER, "off", true, true);
                  bootTempSetPowerOffRetries = 0;
                  bootTempSetStepAtMs = now;
                  bootTempSetSyncState = BOOT_TEMPSET_WAIT_POWER_OFF;
                  break;
                }

                bootTempSetSyncState = BOOT_TEMPSET_DONE;
                break;
              }

              // Read window after entering setpoint UI; then finish gracefully.
              if (timeDiff(now, bootTempSetStepAtMs) >= 1500)
              {
                if (bootTempSetRestorePowerOff)
                {
                  pureSpaIO.setPowerOn(false);
                  mqttClient.publish(MQTT_TOPIC::POWER, "off", true, true);
                  bootTempSetPowerOffRetries = 0;
                  bootTempSetStepAtMs = now;
                  bootTempSetSyncState = BOOT_TEMPSET_WAIT_POWER_OFF;
                  break;
                }
                bootTempSetSyncState = BOOT_TEMPSET_DONE;
              }
              break;
            }

            case BOOT_TEMPSET_WAIT_POWER_OFF:
            {
              if (pureSpaIO.isPowerOn() == false)
              {
                mqttClient.publish(MQTT_TOPIC::POWER, "off", true, true);
                bootTempSetSyncState = BOOT_TEMPSET_DONE;
                break;
              }

              if (timeDiff(now, bootTempSetStepAtMs) >= 1200)
              {
                if (bootTempSetPowerOffRetries < 12)
                {
                  pureSpaIO.setPowerOn(false);
                  mqttClient.publish(MQTT_TOPIC::POWER, "off", true, true);
                  bootTempSetPowerOffRetries++;
                  bootTempSetStepAtMs = now;
                }
                else
                {
                  // Hard fallback: keep trying until panel actually powers off.
                  pureSpaIO.setPowerOn(false);
                  mqttClient.publish(MQTT_TOPIC::POWER, "off", true, true);
                  bootTempSetStepAtMs = now;
                }
              }
              break;
            }

            case BOOT_TEMPSET_DONE:
            default:
              break;
          }
        }
      }

      // Power-on probe: after boot temp discovery is finished, if user turns spa ON and tempSet
      // is still unknown, do a single arrow press to show setpoint (no full boot sequence).
      {
        const uint8 pwrEdge = pureSpaIO.isPowerOn();
        if (pwrEdge == PureSpaIO::UNDEF::BOOL)
        {
          // keep prev until power state is known
        }
        else if (pwrEdge == false)
        {
          prevSpaPowerOnSample = false;
          prevSpaPowerOnValid = true;
          powerOnTempProbeScheduledAtMs = 0;
        }
        else
        {
          const bool rising =
            prevSpaPowerOnValid && !prevSpaPowerOnSample;
          if (rising
              && mqttClient.isConnected()
              && pureSpaIO.isOnline()
              && bootTempSetSyncState == BOOT_TEMPSET_DONE
              && pureSpaIO.getAuthoritativeDesiredWaterTempCelsius() == PureSpaIO::UNDEF::INT
              && pendingWaterSetTempC == PureSpaIO::UNDEF::INT)
          {
            powerOnTempProbeScheduledAtMs = now;
          }
          prevSpaPowerOnSample = true;
          prevSpaPowerOnValid = true;
        }
      }

      if (powerOnTempProbeScheduledAtMs != 0
          && timeDiff(now, powerOnTempProbeScheduledAtMs) >= POWER_ON_TEMP_PROBE_DELAY_MS)
      {
        powerOnTempProbeScheduledAtMs = 0;
        if (mqttClient.isConnected()
            && pureSpaIO.isOnline()
            && pureSpaIO.isPowerOn() == true
            && pureSpaIO.getAuthoritativeDesiredWaterTempCelsius() == PureSpaIO::UNDEF::INT
            && pendingWaterSetTempC == PureSpaIO::UNDEF::INT
            && bootTempSetSyncState == BOOT_TEMPSET_DONE)
        {
          pureSpaIO.markTempDisplayDisturbance();
          if (pureSpaIO.requestSetTempDisplay())
          {
            delay(300);
            yield();
            const int t = pureSpaIO.getDesiredWaterTempCelsius();
            if (t != PureSpaIO::UNDEF::INT)
            {
              pureSpaIO.setAuthoritativeDesiredCelsius(t);
              mqttClient.publish(MQTT_TOPIC::WATER_SET, String(t), true, true);
            }
          }
        }
      }

      delay(20);
    }
  }
  else
  {
    prevMqttConnected = false;
    if (initialized)
    {
      pureSpaIO.loop();
    }
    else if (g_rtcRecovery.heaterPausedForBoot == 1 && now >= 25000UL)
    {
      // Failsafe: if home Wi-Fi is unreachable for 25s after a Stage B heater-pause boot,
      // resume the heater anyway so the hot tub never stays unheated during a router outage.
      setCpuFrequencyMhz(160);
      pureSpaIO.setup(language);
      pureSpaIO.loop();
      if (pureSpaIO.isOnline())
      {
        g_rtcRecovery.heaterPausedForBoot = 0;
        if (pureSpaIO.isPowerOn() == true && pureSpaIO.isHeaterOn() == false)
        {
          pureSpaIO.setHeaterOn(true);
        }
      }
    }

    if (!disconnectTime)
    {
      disconnectTime = now;
      lastWifiReconnectAttemptMs = now;
    }
    else
    {
      const int apClients = webConfig.isFallbackAPActive() ? (int)WiFi.softAPgetStationNum() : 0;

      // Gentle STA reconnect every 30 s when no user is actively connected to the fallback AP
      if (hasConfiguredWifiSsid()
          && apClients == 0
          && timeDiff(now, lastWifiReconnectAttemptMs) >= CONFIG::WIFI_RECONNECT_INTERVAL_MS)
      {
        lastWifiReconnectAttemptMs = now;
        g_wifiReconnectCount++;
        WiFi.reconnect();
      }

      // Self-healing restart after 10 minutes without Wi-Fi, never interrupting an active fallback-AP session
      if (apClients == 0 && timeDiff(now, disconnectTime) > CONFIG::WIFI_MAX_DISCONNECT_DURATION)
      {
        DEBUG_MSG("restarting ... (no WiFi connection for 10 minutes)\n");
        ESP.restart();
      }
    }

    delay(20);
  }
}