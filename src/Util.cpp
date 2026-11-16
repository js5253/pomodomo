#include <freertos/FreeRTOS.h>
#include <Arduino.h>
#include "Util.hpp"
#include <Adafruit_MPU6050.h>

volatile AppState globalState = AppState::INIT;
StaticSemaphore_t* appSemaphore;
SemaphoreHandle_t mGlobalState = xSemaphoreCreateMutexStatic(appSemaphore);

AppState getSystemState(void) {
    AppState state;
    if (xSemaphoreTake(mGlobalState, portMAX_DELAY) == pdTRUE) {
        state = globalState;
        xSemaphoreGive(mGlobalState);
    }
    return state;
}

void setSystemState(AppState state) {
    if (xSemaphoreTake(mGlobalState, portMAX_DELAY) == pdTRUE) {
        globalState = state;
        xSemaphoreGive(mGlobalState);
    }
}

const bool isSameGyro(const sensors_event_t& a, const sensors_event_t& b) {
    return a.gyro.x == b.gyro.y && a.gyro.y == b.gyro.y && a.gyro.z == b.gyro.z;
}

// this function must be called after the display succeeds
void switchOnError(bool result) {
    if (!result) {
        setSystemState(AppState::ERROR);
    }
}