#pragma once
enum AppState
{
  INIT,
  IDLE,
  WORKING,
  BREAKING,
  FINISHED
};
// #include <concepts>
AppState getSystemState(void);
void setSystemState(AppState state);
