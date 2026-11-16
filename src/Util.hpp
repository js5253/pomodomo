#pragma once
enum AppState
{
  INIT,
  IDLE,
  WORKING,
  BREAKING,
  FINISHED,
  ERROR
};
// #include <concepts>
AppState getSystemState(void);
void setSystemState(AppState state);

const bool isSameGyro(const sensors_event_t a, const sensors_event_t b);
void switchOnError(bool result);