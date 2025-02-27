/*
   Copyright 2020 Scott Bezek and the splitflap contributors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <esp_task_wdt.h>

// General splitflap includes
#include "config.h"
#include "src/splitflap_module.h"
#include "src/spi_io_config.h"

// ESP32-specific includes
#include "semaphore_guard.h"
#include "splitflap_task.h"
#include "task.h"

SplitflapTask::SplitflapTask(const uint8_t taskCore) : Task{"Splitflap", 8192, 1, taskCore} {
  semaphore_ = xSemaphoreCreateMutex();
  assert(semaphore_ != NULL);
  xSemaphoreGive(semaphore_);
}

SplitflapTask::~SplitflapTask() {
  if (semaphore_ != NULL) {
    vSemaphoreDelete(semaphore_);
  }
}

// TODO: Move to a separate status class?
static void setLedStatus(uint8_t moduleIndex, bool on) {
#ifdef CHAINLINK
  uint8_t groupPosition = moduleIndex % 6;
  uint8_t byteIndex = MOTOR_BUFFER_LENGTH - 1 - moduleIndex/6*4 - (groupPosition < 3 ? 1 : 2);
  uint8_t bitMask = (groupPosition < 3) ? (1 << (4 + groupPosition)) : (1 << (groupPosition - 3));
  if (on) {
    motor_buffer[byteIndex] |= bitMask;
  } else {
    motor_buffer[byteIndex] &= ~bitMask;
  }
#endif
}

static uint8_t loopbackMotorByte(uint8_t loopbackIndex) {
  return MOTOR_BUFFER_LENGTH - 1 - (loopbackIndex / 2) * 4 - (((loopbackIndex % 2) == 0) ? 1 : 2);
}
static uint8_t loopbackMotorBitMask(uint8_t loopbackIndex) {
  return (loopbackIndex % 2) == 0 ? (1 << 7) : (1 << 3);
}
static uint8_t loopbackSensorByte(uint8_t loopbackIndex) {
  return loopbackIndex / 2;
}
static uint8_t loopbackSensorBitMask(uint8_t loopbackIndex) {
  return (loopbackIndex % 2) == 0 ? 1 << 6 : 1 << 7;
}

void SplitflapTask::run() {
    esp_err_t result = esp_task_wdt_add(NULL);
    ESP_ERROR_CHECK(result);

    initialize_modules();

    // Initialize shift registers before turning on shift register output-enable
    motor_sensor_io();
    pinMode(OUTPUT_ENABLE_PIN, OUTPUT);
    digitalWrite(OUTPUT_ENABLE_PIN, LOW);

    // TODO: Move to serial task
    for (uint8_t i = 0; i < NUM_MODULES; i++) {
      recv_buffer[i] = 0;
    }
    Serial.print("\n\n\n");
    Serial.print("{\"type\":\"init\", \"num_modules\":");
    Serial.print(NUM_MODULES);
    Serial.print("}\n");

#ifdef CHAINLINK

    bool loopback_error = false;

    // Turn one loopback bit on at a time and make sure only that loopback bit is set
    for (uint8_t loop_out_index = 0; loop_out_index < NUM_LOOPBACKS; loop_out_index++) {
      motor_buffer[loopbackMotorByte(loop_out_index)] = loopbackMotorBitMask(loop_out_index);
      motor_sensor_io();
      motor_sensor_io();
      for (uint8_t loop_in_index = 0; loop_in_index < NUM_LOOPBACKS; loop_in_index++) {
        uint8_t expected_bit_mask = (loop_out_index == loop_in_index) ? loopbackSensorBitMask(loop_in_index) : 0;
        uint8_t actual_bit_mask = sensor_buffer[loopbackSensorByte(loop_in_index)] & loopbackSensorBitMask(loop_in_index);
        if (actual_bit_mask != expected_bit_mask) {
          // TODO: do something with loopback errors
          loopback_error = true;
          Serial.printf("Bad loopback. Set loopback %u but found unexpected value at loopback %u\n", 
              loop_out_index,
              loop_in_index
          );
        }
      }
      motor_buffer[loopbackMotorByte(loop_out_index)] = 0;
    }

    // Turn off all motors, leds, and loopbacks; make sure all loopback inputs read 0
    memset(motor_buffer, 0, MOTOR_BUFFER_LENGTH);
    motor_sensor_io();
    motor_sensor_io();
    for (uint8_t i = 0; i < NUM_LOOPBACKS; i++) {
      if ((sensor_buffer[loopbackSensorByte(i)] & loopbackSensorBitMask(i)) != 0) {
        // TODO: do something with loopback errors
        loopback_error = true;
        Serial.printf("Bad loopback at index %u - should have been 0\n", i);
      }
    }

    if (loopback_error) {
      // Loop forever
      while(1) {
          updateStateCache();
          ESP_ERROR_CHECK(esp_task_wdt_reset());
      }
    }

    for (uint8_t r = 0; r < 3; r++) {
      for (uint8_t i = 0; i < NUM_MODULES; i++) {
        setLedStatus(i, 1);
        motor_sensor_io();
        delay(10);
        setLedStatus(i, 0);
        motor_sensor_io();
      }
      result = esp_task_wdt_reset();
      ESP_ERROR_CHECK(result);
      delay(500);
    }
#endif

    for (uint8_t i = 0; i < NUM_MODULES; i++) {
        modules[i]->Init();
        modules[i]->GoHome();
    }

    while(1) {
        runUpdate();
        updateStateCache();

        result = esp_task_wdt_reset();
        ESP_ERROR_CHECK(result);
    }
}

void SplitflapTask::updateStateCache() {
    SplitflapState new_state;
    for (uint8_t i = 0; i < NUM_MODULES; i++) {
      new_state.modules[i].flapIndex = modules[i]->GetCurrentFlapIndex();
      new_state.modules[i].state = modules[i]->state;
    }
    if (memcmp(&state_cache_, &new_state, sizeof(state_cache_))) {
        SemaphoreGuard lock(semaphore_);
        memcpy(&state_cache_, &new_state, sizeof(state_cache_));
    }
}

void SplitflapTask::showString(const char* str, uint8_t length) {
    SemaphoreGuard lock(semaphore_);
    pending_move_response = true;
    Serial.print("{\"type\":\"move_echo\", \"dest\":\"");
    Serial.flush();
    for (uint8_t i = 0; i < length; i++) {
        int8_t index = findFlapIndex(str[i]);
        if (index != -1) {
            if (FORCE_FULL_ROTATION || index != modules[i]->GetTargetFlapIndex()) {
            modules[i]->GoToFlapIndex(index);
            }
        }
        Serial.write(str[i]);
        if (i % 8 == 0) {
            Serial.flush();
        }
    }
    Serial.print("\"}\n");
    Serial.flush();
}

void SplitflapTask::runUpdate() {
    uint32_t iterationStartMillis = millis();

    uint32_t flashStep = iterationStartMillis / 200;
    uint32_t flashGroup = (flashStep % 16) / 2;
    uint8_t flashPhase = flashStep % 2;

    boolean all_idle = true;
    boolean all_stopped = true;

    if (!sensor_test_) {
      for (uint8_t i = 0; i < NUM_MODULES; i++) {
        modules[i]->Update();
        bool is_idle = modules[i]->state == PANIC
          || modules[i]->state == STATE_DISABLED
          || modules[i]->state == LOOK_FOR_HOME
          || modules[i]->state == SENSOR_ERROR
          || (modules[i]->state == NORMAL && modules[i]->current_accel_step == 0);

        bool is_stopped = modules[i]->state == PANIC
          || modules[i]->state == STATE_DISABLED
          || modules[i]->current_accel_step == 0;

        setLedStatus(i, flashGroup < modules[i]->state && flashPhase == 0);

        all_idle &= is_idle;
        all_stopped &= is_stopped;
      }
      if (all_stopped && !was_stopped) {
        stopped_at_millis = iterationStartMillis;
      }
      was_stopped = all_stopped;
      motor_sensor_io();
    } else {
      // Read sensor state
      motor_sensor_io();

      for (uint8_t i = 0; i < NUM_MODULES; i++) {
        setLedStatus(i, modules[i]->GetHomeState());
      }

      // Output LED state
      motor_sensor_io();
    }

#if INA219_POWER_SENSE
    if (iterationStartMillis - lastCurrentReadMillis > 100) {
      currentmA = powerSense.getCurrent_mA();
      if (currentmA > NUM_MODULES * 250) {
        disableAll("Over current");
      } else if (all_stopped && iterationStartMillis - stopped_at_millis > 100 && currentmA >= 3) {
        disableAll("Unexpected current");
      }
      lastCurrentReadMillis = iterationStartMillis;
    }
#endif

    if (all_idle) {

#if INA219_POWER_SENSE
        float voltage = powerSense.getBusVoltage_V();
        if (voltage > 14) {
          disableAll("Over voltage");
        } else if (voltage < 10) {
          disableAll("Under voltage");
        }
        // dtostrf(voltage, 6, 3, voltageBuf);
        // snprintf(displayBuffer, sizeof(displayBuffer), "%sV", voltageBuf);
        // display.setCursor(84, 0);
        // display.print(displayBuffer);

        // dtostrf(currentmA / 1000, 6, 3, currentBuf);
        // snprintf(displayBuffer, sizeof(displayBuffer), "%sA", currentBuf);
        // display.setCursor(84, 8);
        // display.print(displayBuffer);
#endif

      if (pending_no_op && all_stopped) {
        Serial.print("{\"type\":\"no_op\"}\n");
        Serial.flush();
        pending_no_op = false;
      }
      if (pending_move_response && all_stopped) {
        pending_move_response = false;
        dumpStatus();
      }

      while (Serial.available() > 0) {
        int b = Serial.read();
        if (b == '%' && all_stopped) {
          sensor_test_ = !sensor_test_;
          Serial.print("{\"type\":\"sensor_test\", \"enabled\":");
          Serial.print(sensor_test_ ? "true" : "false");
          Serial.print("}\n");
        } else if (sensor_test_ == false) {
          switch (b) {
            case '@':
              for (uint8_t i = 0; i < NUM_MODULES; i++) {
                modules[i]->ResetErrorCounters();
                modules[i]->GoHome();
              }
              break;
            case '#':
              pending_no_op = true;
              break;
            case '=':
              recv_count = 0;
              break;
            case '\n':
                showString(recv_buffer, recv_count);
                break;
            default:
              if (recv_count > NUM_MODULES - 1) {
                break;
              }
              recv_buffer[recv_count] = b;
              recv_count++;
              break;
          }
        }
      }
    }
}

int8_t SplitflapTask::findFlapIndex(uint8_t character) {
    for (int8_t i = 0; i < NUM_FLAPS; i++) {
        if (character == flaps[i]) {
          return i;
        }
    }
    return -1;
}

void SplitflapTask::dumpStatus() {
  Serial.print("{\"type\":\"status\", \"modules\":[");
  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    Serial.print("{\"state\":\"");
    switch (modules[i]->state) {
      case NORMAL:
        Serial.print("normal");
        break;
      case LOOK_FOR_HOME:
        Serial.print("look_for_home");
        break;
      case SENSOR_ERROR:
        Serial.print("sensor_error");
        break;
      case PANIC:
        Serial.print("panic");
        break;
      case STATE_DISABLED:
        Serial.print("disabled");
        break;
    }
    Serial.print("\", \"flap\":\"");
    Serial.write(flaps[modules[i]->GetCurrentFlapIndex()]);
    Serial.print("\", \"count_missed_home\":");
    Serial.print(modules[i]->count_missed_home);
    Serial.print(", \"count_unexpected_home\":");
    Serial.print(modules[i]->count_unexpected_home);
    Serial.print("}");
    if (i < NUM_MODULES - 1) {
      Serial.print(", ");
    }
  }
  Serial.print("]}\n");
  Serial.flush();
}

SplitflapState SplitflapTask::getState() {
    SemaphoreGuard lock(semaphore_);
    return state_cache_;
}

// void SplitflapTask::disableAll(const char* message) {
//   for (uint8_t i = 0; i < NUM_MODULES; i++) {
//     modules[i]->Disable();
//   }
//   motor_sensor_io();

//   if (disabled) {
//     return;
//   }
//   disabled = true;

//   Serial.println("#### DISABLED! ####");
//   Serial.println(message);
// }
