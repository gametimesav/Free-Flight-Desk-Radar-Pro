#pragma once

#include <Arduino.h>

namespace netlock {

inline SemaphoreHandle_t& mutex_ref() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

class Guard {
public:
  Guard() {
    SemaphoreHandle_t m = mutex_ref();
    if (m) {
      xSemaphoreTake(m, portMAX_DELAY);
      locked_ = true;
    }
  }

  ~Guard() {
    if (locked_) {
      xSemaphoreGive(mutex_ref());
    }
  }

private:
  bool locked_ = false;
};

}  // namespace netlock
