#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "pins.h"
#include "mc.h"

#define SUCCESSIVE_FULL_INDICATIONS_FOR_BEEP 4
#define SUCCESSIVE_FULL_INDICATIONS_FOR_MOTOR_OFF 5
    
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

/* Reading the GPIO and counting a single high as `full` and a single low as
   `not full` is too unreliable.
   Even when the tank is full, we get the occasional not-full reading.
   And even when the tank is not full, we get a few spurious tank-full readings in
   succession.
   To address this, we average out a few readings by maintaining a circular buffer
   of booleans.
   Once the number of `trues` in the circular buffer crosses a threshold, we treat
   that as a SINGLE tank-full.

   We use `n` successive tank-full indications derived using this method to
   sound the beep.
   We use `m` successive tank-full indications derived using this method to
   turn the motor off.
   Where n = SUCCESSIVE_FULL_INDICATIONS_FOR_BEEP and m is
   SUCCESSIVE_FULL_INDICATIONS_FOR_MOTOR_OFF */
   
#define FULL_REPORTS_CIRC_BUFF_SIZE 10
#define FULL_REPORTS_THRESHOLD 4
static bool full_reports_circ_buff[FULL_REPORTS_CIRC_BUFF_SIZE];
static unsigned int full_reports_circ_buff_index = 0;

/* We want to log the full reports for debugging reasons, but since that
   is updated every second it generates a lot of unnecessary logs. So
   we log it only when its value has changed.
   It's initially set to an impossible value. */
static unsigned int full_reports_last_logged = FULL_REPORTS_CIRC_BUFF_SIZE + 1;

/* Take one GPIO reading, add it to the circular buffer, average out the
   circular buffer readings, and return whether the threshold was crossed. */
static bool update_and_report_tank_full (bool is_reporting_full_now) {
  unsigned int i, full_reports;
  
  full_reports_circ_buff[full_reports_circ_buff_index++] = is_reporting_full_now;
  full_reports_circ_buff_index %= FULL_REPORTS_CIRC_BUFF_SIZE;

  full_reports = 0;
  for (i = 0; i < FULL_REPORTS_CIRC_BUFF_SIZE; i++) {
    if (full_reports_circ_buff[i]) {
      full_reports++;
    }
  }

  if (full_reports != full_reports_last_logged) {
    ESP_LOGI(LOG_TAG, "GPIO now = %s, full reports in last %u seconds = %u",
	     is_reporting_full_now ? "full" : "not full",
	     FULL_REPORTS_CIRC_BUFF_SIZE, full_reports);
    full_reports_last_logged = full_reports;
  }
  
  if (full_reports >= FULL_REPORTS_THRESHOLD) {
    return true;
  } else {
    return false;
  }
}

/* Utility function to clear out the full reports circular buffer */
static void clear_full_reports (void) {
  unsigned int i;

  for (i = 0; i < FULL_REPORTS_CIRC_BUFF_SIZE; i++) {
    full_reports_circ_buff[i] = false;
  }
  full_reports_circ_buff_index = 0;
}

static bool is_motor_running_now (struct mc_task_args_t_ *mc_task_args) {
  EventBits_t bits;
  bits = xEventGroupGetBits(mc_task_args->mc_event_group);
  if ((bits & EVENT_MOTOR_RUNNING) == 0) {
    return false;
  } else {
    return true;
  }
}

void oh_tank_level_task (void *param) {
  bool beeping_now, motor_was_running, is_reporting_full_now;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;
  unsigned int successive_full_indications = 0;

  clear_full_reports();
  
  beeping_now = false;
  motor_was_running = false;
  successive_full_indications = 0;
  
  while (pdTRUE) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!is_motor_running_now(mc_task_args)) {
      /* If we are beeping, stop it because the motor is now off */
      if (beeping_now) {
	ESP_LOGI(LOG_TAG, "Turning beep off now because motor is not running");
	beep_off(mc_task_args);
	beeping_now = false;
      }
      if (motor_was_running) {
	motor_was_running = false;
	ESP_LOGI(LOG_TAG, "Motor was running, now stopped");
      }
    } else {
      if (!motor_was_running) {
	motor_was_running = true;
	successive_full_indications = 0;
	clear_full_reports();
	ESP_LOGI(LOG_TAG, "Motor was stopped, now running");
      }

      /* Enable the water level sensor and wait for 500ms*/
      gpio_set_level(WATER_LEVEL_ENABLE_OUT, 1);
      vTaskDelay(pdMS_TO_TICKS(500));

      /* Read the water level and disable the water level sensor */
      is_reporting_full_now = gpio_get_level(WATER_LEVEL_IN);
      gpio_set_level(WATER_LEVEL_ENABLE_OUT, 0);
      
      if (update_and_report_tank_full(is_reporting_full_now)) {
	successive_full_indications++;
      } else {
	successive_full_indications = 0;
      }

      if (successive_full_indications == SUCCESSIVE_FULL_INDICATIONS_FOR_BEEP) {
	ESP_LOGI(LOG_TAG, "Successive tank full indications have crossed the beep "
		 "threshold");
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
