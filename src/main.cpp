#include <Arduino.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <optional>
#include <driver/timer.h>
#include <Preferences.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <driver/i2s.h>
#include "Util.hpp"
#include <Audio.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define I2S_DOUT 22
#define I2S_BCLK 26
#define I2S_LRC 25

uint8_t gyroQueueStorageArea[10 * sizeof(GyroMessage)];
StaticQueue_t gyroStaticQueue;

uint8_t elapsedStorageArea[sizeof(bool)];
uint8_t leftTimeStorageArea[sizeof(bool)];
uint8_t minutesStorageArea[sizeof(bool)];


// CONFIGURE THESE VALUES!

struct OrientationMinuteMappings
{
  int normal;
  int count_90;
  int inverted;
  int clock_90;
};

volatile int timeElapsed = 0;
volatile int timeLeft = 0;

QueueHandle_t qOrientationChange;

StaticQueue_t timeStaticQueue;

StaticSemaphore_t* elapsedTimeSemaphore;
StaticSemaphore_t* leftTimeSemaphore;
StaticSemaphore_t* minutesSemaphore;

SemaphoreHandle_t mElapsedTime = xSemaphoreCreateMutexStatic(elapsedTimeSemaphore);
SemaphoreHandle_t mLeftTime = xSemaphoreCreateMutexStatic(leftTimeSemaphore);
SemaphoreHandle_t mMinutes = xSemaphoreCreateMutexStatic(minutesSemaphore);

TaskHandle_t tInteraction;
TaskHandle_t tIMU;
TaskHandle_t tNetwork;
TaskHandle_t tTimer;

Adafruit_MPU6050 mpu;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
hw_timer_t *myTimer = NULL;
AsyncWebServer server(80);
Preferences preferences;
Audio audio;

const char *SSID = "POMODORMO";
const char *PASSWORD = "POMODORMO";

enum GyroMessageType
{
  CYCLE_FLIPPED_NORMAL,
  CYCLE_FLIPPED_COUNT,
  CYCLE_FLIPPED_INV,
  CYCLE_FLIPPED_CLOCK
};
struct GyroMessage
{
  GyroMessageType msg;
};

OrientationMinuteMappings minutes = {
    .normal = 5,
    .count_90 = 10,
    .inverted = 20,
    .clock_90 = 55,
};

void ARDUINO_ISR_ATTR onTimer()
{
  xSemaphoreTake(mLeftTime, portMAX_DELAY);
  xSemaphoreTake(mElapsedTime, portMAX_DELAY);
  --timeLeft;
  ++timeElapsed;
  xSemaphoreGive(mElapsedTime);
  xSemaphoreGive(mLeftTime);
}
void ARDUINO_ISR_ATTR makeIdle()
{
  setSystemState(AppState::IDLE);
}

void tfInteraction(void *params)
{
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 10);

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(17);
  audio.connecttoFS(LittleFS, "/alarm.wav");
  std::optional<int> prevDisplayTime = std::nullopt;
  while (true)
  {
    AppState state = getSystemState();
    switch (state)
    {
    case INIT:
      break;
    case WORKING:
      xSemaphoreTake(mElapsedTime, portMAX_DELAY);
      if (prevDisplayTime != timeElapsed)
      {
        display.clearDisplay();
        display.printf("WORKING: %d", mElapsedTime);
      }
      prevDisplayTime = timeElapsed;
      xSemaphoreGive(mElapsedTime);
      break;

    case BREAKING:
      xSemaphoreTake(mElapsedTime, portMAX_DELAY);
      if (prevDisplayTime != timeElapsed)
      {
        display.clearDisplay();
        display.printf("BREAK: %d", mElapsedTime);
      }
      prevDisplayTime = timeElapsed;
      xSemaphoreGive(mElapsedTime);
      break;
    case FINISHED:
      display.clearDisplay();
      display.printf("TIME!");
      break;
    case ERROR:
      display.clearDisplay();
      display.printf("ERROR");
    case IDLE:
      // do something?
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
void tfIMU(void *params)
{
  // setup
  sensors_event_t gyro{};

  std::optional<sensors_event_t> prevGyro = std::nullopt;

  mpu.setGyroRange(MPU6050_RANGE_1000_DEG);
  mpu.setAccelerometerStandby(true, true, true);
  mpu.setTemperatureStandby(true);

  while (true)
  {
    AppState state = getSystemState();
    switch (state)
    {
    case INIT:
    [[fallthrough]]
    case WORKING:
    [[fallthrough]]
    case IDLE:
    [[fallthrough]]
    case BREAKING:
      mpu.getGyroSensor()->getEvent(&gyro);
      if (!prevGyro || !isSameGyro(*prevGyro, gyro))
        ;
      {

        int degree = gyro.gyro.roll;
        /// FIXME: add correct axis and degree rating
        GyroMessageType msg = GyroMessageType::CYCLE_FLIPPED_NORMAL;
        if (degree < 90)
        {
          msg = GyroMessageType::CYCLE_FLIPPED_CLOCK;
        }
        else if (
            degree < 0)
        {
          msg = GyroMessageType::CYCLE_FLIPPED_COUNT;
        }
        else if (degree < 90)
        {
          msg = GyroMessageType::CYCLE_FLIPPED_INV;
        }
        xQueueSend(qOrientationChange, (void *)new GyroMessage{.msg = msg}, 0);
      }
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void tfTimer(void *params)
{
  GyroMessage *messageEvent;
  while (true)
  {
    AppState state = getSystemState();
    xSemaphoreTake(mLeftTime, portMAX_DELAY);
    if (xQueueReceive(qOrientationChange, &(messageEvent), portMAX_DELAY) == pdPASS)
    {
      // reset timer

      switch (messageEvent->msg)
      {
      case CYCLE_FLIPPED_NORMAL:
        timeLeft = minutes.normal;
        break;
      case CYCLE_FLIPPED_COUNT:
        timeLeft = minutes.count_90;
        break;
      case CYCLE_FLIPPED_INV:
        timeLeft = minutes.inverted;
        break;
      case CYCLE_FLIPPED_CLOCK:
        timeLeft = minutes.clock_90;
        break;
      }
      state = AppState::WORKING;
      printf("Time Change!");
      myTimer = timerBegin(TIMER_0); /// TODO: check if this is an appropriate prescaler value
      timerAttachInterrupt(myTimer, &onTimer);
      timerWrite(myTimer, 1000000);
      timerStart(myTimer);

      xSemaphoreTake(mElapsedTime, portMAX_DELAY);
      timeElapsed = 0;
      xSemaphoreGive(mElapsedTime);
      xSemaphoreGive(mLeftTime);
      delete messageEvent;
    };
    switch (state)
    {
    case INIT:
      break;
    case IDLE:
      break;
    case WORKING:
      if (timeLeft == 0)
      {
        timerStop(myTimer);
        timerStart(myTimer);
        state = BREAKING;
      }
      break;
    case BREAKING:
      if (timeLeft == 0)
      {
        timerStop(myTimer);
        state = FINISHED;
      }
      break;

    case FINISHED:
      timerAttachInterrupt(myTimer, &makeIdle);
      timerWrite(myTimer, 1000000);
      timerStart(myTimer);

      // here, make an alarm later on
      break;
    }
    xSemaphoreGive(mLeftTime);

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void onRequest(AsyncWebServerRequest *req)
{
  // Handle Unknown Request
  req->send(404);
}
void onConfigChangeRequest(AsyncWebServerRequest *req)
{
  const AsyncWebParameter* counterTime = req->getParam("counterTime");
  const AsyncWebParameter* normalTime = req->getParam("normalTime");
  const AsyncWebParameter* clockTime = req->getParam("clockTime");
  const AsyncWebParameter* invertTime = req->getParam("invertTime");
  if (counterTime != nullptr && normalTime != nullptr && clockTime != nullptr && invertTime != nullptr)
  {

    xSemaphoreTake(mMinutes, portMAX_DELAY);
    minutes = {
        .normal = normalTime->value().toInt() || minutes.normal,
        .count_90 = counterTime->value().toInt() || minutes.count_90,
        .inverted = invertTime->value().toInt() || minutes.inverted,
        .clock_90 = clockTime->value().toInt() || minutes.clock_90,
    };
    xSemaphoreGive(mMinutes);
  }
  req->send(200);
}

void tfNetwork(void *params)
{
  /// TODO: check that AsyncWebServer works well this way/under FreeRTOS
  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void setup()
{
  // do peripheral setup
  Serial.begin(115200);
  switchOnError(display.begin());
  switchOnError(mpu.begin());
  switchOnError(LittleFS.begin());
  ESP_ERROR_CHECK(LittleFS.begin(true));
  WiFi.mode(WIFI_STA);
  switchOnError(WL_CONNECT_FAILED != WiFi.begin(SSID, PASSWORD));
  display.clearDisplay();

  switchOnError(elapsedTimeSemaphore);
  switchOnError(leftTimeSemaphore);
  switchOnError(minutesSemaphore);

  server.onNotFound(onRequest);
  server.on("/config", onConfigChangeRequest);
  server.serveStatic("/index.html", LittleFS, "/www/index.html").setDefaultFile("/www/index.html");
  server.begin();

  // end peripheral setup

  setCpuFrequencyMhz(80);

  // get default minute mappings
  switchOnError(preferences.begin("config", false));
  minutes = OrientationMinuteMappings{
      .normal = preferences.getInt("normal", 5),
      .count_90 = preferences.getInt("count_90", 10),
      .inverted = preferences.getInt("inverted", 20),
      .clock_90 = preferences.getInt("clock_90", 55),
  };
  preferences.end();
  // end default minute mappings
  qOrientationChange = xQueueCreateStatic(500, sizeof(GyroMessageType), gyroQueueStorageArea, &gyroStaticQueue);

  xTaskCreate(tfInteraction, "InteractionTask", 1000, NULL, 3, &tInteraction);
  xTaskCreate(tfIMU, "IMUTask", 1000, NULL, 2, &tIMU);
  xTaskCreate(tfNetwork, "NetworkTask", 1000, NULL, 4, &tNetwork);
  xTaskCreate(tfTimer, "TimerTask", 1000, NULL, 1, &tTimer);
}
void loop()
{
  // do nothing; we're using FreeRTOS
}