#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "pins.h"
#include "mc.h"

#define SUCCESSIVE_FULL_INDICATIONS_FOR_BEEP 10
#define SUCCESSIVE_FULL_INDICATIONS_FOR_MOTOR_OFF 15
    
/* This is a global because the HTTP server reads it */
unsigned int oh_tank_full_seconds;
static char const *LOG_TAG = "mc|oh_tank_level";

static void beep_on (struct mc_task_args_t_ *mc_task_args) {
  static bool on = true; /* I think this needs to be static */
  if (pdTRUE != xQueueSend(mc_task_args->beep_q, (void *) &on, pdMS_TO_TICKS(1000))) {
    ESP_LOGE(LOG_TAG, "Failed to enqueue beep ON");
  }
}

static void beep_off (struct mc_task_args_t_ *mc_task_args) {
  static bool on = false; /* I think this needs to be static */
  if (pdTRUE != xQueueSend(mc_task_args->beep_q, (void *) &on, pdMS_TO_TICKS(1000))) {
    ESP_LOGE(LOG_TAG, "Failed to enqueue beep OFF");
  }
}

static void motor_off (struct mc_task_args_t_ *mc_task_args) {
  static bool on = false; /* I think this needs to be static */
  if (pdTRUE != xQueueSend(mc_task_args->motor_on_off_q, (void *) &on, pdMS_TO_TICKS(1000))) {
    ESP_LOGE(LOG_TAG, "Failed to enqueue motor OFF");
  }
}

void oh_tank_level_task (void *param) {
  bool beeping_now;
  EventBits_t bits;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;
  unsigned int successive_full_indications;

  successive_full_indications = 0;
  beeping_now = false;
  oh_tank_full_seconds = 0;
  
  while (pdTRUE) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Check if the tank is full. */
    if (!gpio_get_level(WATER_LEVEL_IN)) {
      /* Tank is not full. Reset the state variables, turn off the beep if it is on */
      if (successive_full_indications != 0) {
	successive_full_indications = 0;
	ESP_LOGI(LOG_TAG, "Tank is not full, successive_full_indications = 0");
      }
      oh_tank_full_seconds = 0;

      if (beeping_now) {
	ESP_LOGI(LOG_TAG, "Turning beep off now because tank level has fallen");
	beep_off(mc_task_args);
	beeping_now = false;
      }
    } else {
      /* Tank is full. If the global `oh_tank_full_seconds` is non-zero, increment it so
	 the http server can report how long the tank has been full for. Note that at this
	 point we haven't yet checked if the motor is running; we update the
	 oh_tank_full_seconds regardless of that */
      if (oh_tank_full_seconds) {
	oh_tank_full_seconds++;
      }

      bits = xEventGroupGetBits(mc_task_args->mc_event_group);
      /* If the motor is not running at this point, stop beeping now if we're beeping */
      if ((bits & EVENT_MOTOR_RUNNING) == 0) {
	if (beeping_now) {
	  ESP_LOGI(LOG_TAG, "Turning beep off now because motor is not running");
	  beep_off(mc_task_args);
	  beeping_now = false;
	}
	/* We track `successive_full_indications` only when the motor is running */
	if (successive_full_indications != 0) {
	  successive_full_indications = 0;
	  ESP_LOGI(LOG_TAG, "Motor is not running, successive_full_indications = 0");
	}

	/* For the case where the tank is full and the motor is not running, we have
	   nothing else to do */
	continue;
      }

      successive_full_indications++;
      ESP_LOGI(LOG_TAG, "Tank is full, motor is running, successive_full_indications = %u",
	       successive_full_indications);
	
      if (successive_full_indications == SUCCESSIVE_FULL_INDICATIONS_FOR_BEEP) {
	ESP_LOGI(LOG_TAG, "Successive tank full indications have crossed the beep "
		 "threshold");
	/* This is the value returned in the httpd status call. It is maintained
	   independent of the `successive_full_indications` variable, and is set to
	   non-zero only when we start beeping */
	if (!oh_tank_full_seconds) {
	  oh_tank_full_seconds = successive_full_indications;
	}
	beep_on(mc_task_args);
	beeping_now = true;
      }
      if (successive_full_indications == SUCCESSIVE_FULL_INDICATIONS_FOR_MOTOR_OFF) {
	ESP_LOGI(LOG_TAG, "Successive tank full indications have crossed the motor"
		 " off threshold");
	motor_off(mc_task_args);
      }
    }
  }
}
