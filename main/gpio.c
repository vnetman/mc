#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "pins.h"

static char const *LOG_TAG = "mc|gpio";

void init_gpio_pins (void) {
  gpio_config_t gpio_conf;
  
  memset(&gpio_conf, 0, sizeof(gpio_conf));
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = (1ull << BEEP_OUT) | (1ull << ERR_STATUS_OUT) |
    (1ull << MOTOR_OUT);
  
  if (gpio_config(&gpio_conf) != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Unable to init GPIOs for outputs");
    return;
  }

  memset(&gpio_conf, 0, sizeof(gpio_conf));
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_INPUT;
  gpio_conf.pin_bit_mask = (1ull << MOTOR_RUNNING_SENSE_IN);
  gpio_conf.pull_down_en = 1;

  if (gpio_config(&gpio_conf) != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Unable to init GPIO for input");
  }
}
