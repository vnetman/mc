#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "pins.h"

static char const *LOG_TAG = "mc|gpio";

static void set_motor_out_line_high (void) {
  /* The relay we use for the MOTOR_OUT function is Active Low, i.e. contacts
     are in Normal state when the GPIO is High, and click to non-Normal state
     when GPIO goes low. At bootup we want contacts to be in their Normal states
     so we set the MOTOR_OUT GPIO line to High.*/
  gpio_set_level(MOTOR_OUT, 1);
}

void init_gpio_pins (void) {
  gpio_config_t gpio_conf;

  /* Seems a little weird to be doing this BEFORE configuring the GPIO pins,
     but it works. If we don't do this, our relay will click for an instant when
     our board is booting up, which we don't want */
  set_motor_out_line_high();
  
  /* Set up the GPIO output pins */
  memset(&gpio_conf, 0, sizeof(gpio_conf));
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = (1ull << BEEP_OUT) | (1ull << ERR_STATUS_OUT) |
    (1ull << MOTOR_OUT);
  gpio_conf.pull_down_en = 0;
  gpio_conf.pull_up_en = 0;  
  if (gpio_config(&gpio_conf) != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Unable to init GPIOs for outputs");
    return;
  }

  /* May not be needed because we've already done it. But it looks like a
     polite thing to do now, after formally configuring the GPIO outputs */
  set_motor_out_line_high();
  
  memset(&gpio_conf, 0, sizeof(gpio_conf));
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_INPUT;
  gpio_conf.pin_bit_mask = (1ull << MOTOR_RUNNING_SENSE_IN) | (1ull << WATER_LEVEL_IN);
  gpio_conf.pull_down_en = 1;

  if (gpio_config(&gpio_conf) != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Unable to init GPIO for input");
  }
}
