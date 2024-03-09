#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "pins.h"
#include "mc.h"

/* This is a global because the HTTP server reads it */
bool oh_tank_level_last = false;

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
  EventBits_t bits;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;

  while (pdTRUE) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* Read the Input GPIO to see if the motor is running */
    if (gpio_get_level(WATER_LEVEL_IN) == 0) {
      ESP_LOGI(LOG_TAG, "OH tank is not full");
      oh_tank_level_last = false;
    } else {
      ESP_LOGI(LOG_TAG, "OH tank is full");
      oh_tank_level_last = true;
    }
  }
    

#ifdef ORIGINAL_CODE
  BaseType_t successive_full_indications;
  adc_oneshot_unit_handle_t adc_handle;
  adc_cali_handle_t adc_cali_handle;
  bool adc_calibration_ok, adc_configured, adc_ok;

  bool motor_on, beeping_now;
  EventBits_t bits;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;

  /* Start monitoring */
  successive_full_indications = 0;
  motor_on = false;
  beeping_now = false;
  adc_configured = false;
  adc_ok = false;
  
  while (pdTRUE) {
    if (!motor_on) {
      /* Do nothing until the motor gets turned on */
      ESP_LOGI(LOG_TAG, "oh_tank_level: Waiting for the motor to start running");
      bits = xEventGroupWaitBits(mc_task_args->mc_event_group,
				 EVENT_MOTOR_RUNNING,
				 pdFALSE /* don't clear bit on exit */,
				 pdFALSE /* xWaitForAllBits */,
				 portMAX_DELAY);
      if (bits & EVENT_MOTOR_RUNNING) {
	ESP_LOGI(LOG_TAG, "Motor has started running");
	motor_on = true;
      }
    } else {
      bits = xEventGroupGetBits(mc_task_args->mc_event_group);
      if (bits & EVENT_MOTOR_RUNNING) {
	ESP_LOGD(LOG_TAG, "Motor is running");
	
	if (!adc_ok) {
	  if (!adc_configured) {
	    adc_ok = oh_tank_level_adc_init(&adc_handle, &adc_cali_handle, &adc_calibration_ok);
	    adc_configured = true;
	  }
	}

	if (adc_ok) {
	  oh_tank_level_read_adc(adc_handle, adc_cali_handle, adc_calibration_ok);
	} else {
	  ESP_LOGE(LOG_TAG, "ADC is not functional, not monitoring");
	}
	
	oh_tank_level_voltage_last = 1133; /* for now */

	if (oh_tank_level_voltage_last < 800) {
	  successive_full_indications++;
	  if (successive_full_indications >= 7) {
	    if (successive_full_indications >= 12) {
	      motor_off(mc_task_args);
	    }
	    if (!beeping_now) {
	      beeping_now = true;
	      beep_on(mc_task_args);
	    }
	  }
	} else {
	  successive_full_indications = 0;
	  if (beeping_now) {
	    beeping_now = false;
	    beep_off(mc_task_args);
	  }
	}
      } else {
	ESP_LOGI(LOG_TAG, "Motor is not running");
	motor_on = false;
	oh_tank_level_voltage_last = 0;
	successive_full_indications = 0;
	if (beeping_now) {
	  beeping_now = false;
	  beep_off(mc_task_args);
	}
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif /* ORIGINAL_CODE */
}
