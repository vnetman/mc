#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_log.h"
#include "pins.h"
#include "mc.h"

#define DEFAULT_VREF 1107 /* See below for how this was obtained */

/*
debian@deb12-esp:~/esp-projects/mc$ python3 $IDF_PATH/components/esptool_py/esptool/espefuse.py --port /dev/ttyUSB0 adc_info
espefuse.py vv4.8.dev1
Connecting....
Detecting chip type... Unsupported detection protocol, switching and trying again...
Connecting......
Detecting chip type... ESP32

=== Run "adc_info" command ===
ADC VRef calibration: 1107mV
debian@deb12-esp:~/esp-projects/mc$ 
*/

#define NO_OF_SAMPLES 64

static const adc_channel_t channel = WATER_LEVEL_ADC_CHANNEL;
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
static const adc_unit_t unit = ADC_UNIT_1;
static const adc_atten_t atten = ADC_ATTEN_DB_0; /* 2_5/6/11 */

/* This is a global because the HTTP server reads it */
uint32_t oh_tank_level_voltage_last = 0;

static void beep_on (struct mc_task_args_t_ *mc_task_args) {
  static bool on = true; /* I think this needs to be static */
  if (pdTRUE != xQueueSend(mc_task_args->beep_q, (void *) &on, pdMS_TO_TICKS(1000))) {
    ESP_LOGE(LOG_TAG, "oh_tank_level: Failed to enqueue beep ON");
  }
}

static void beep_off (struct mc_task_args_t_ *mc_task_args) {
  static bool on = false; /* I think this needs to be static */
  if (pdTRUE != xQueueSend(mc_task_args->beep_q, (void *) &on, pdMS_TO_TICKS(1000))) {
    ESP_LOGE(LOG_TAG, "oh_tank_level: Failed to enqueue beep OFF");
  }
}

static void motor_off (struct mc_task_args_t_ *mc_task_args) {
  static bool on = false; /* I think this needs to be static */
  if (pdTRUE != xQueueSend(mc_task_args->motor_on_off_q, (void *) &on, pdMS_TO_TICKS(1000))) {
    ESP_LOGE(LOG_TAG, "oh_tank_level: Failed to enqueue motor OFF");
  }
}

void oh_tank_level_task (void *param) {
  esp_adc_cal_characteristics_t adc_chars = {0};
  esp_adc_cal_value_t val_type;
  uint32_t adc_reading;
  BaseType_t i, successive_full_indications;

  bool motor_on, beeping_now;
  EventBits_t bits;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;

  /* The task is just starting up, init the ADC and characterize it */
  ESP_ERROR_CHECK(adc1_config_width(width));
  ESP_ERROR_CHECK(adc1_config_channel_atten(channel, atten));

  val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, &adc_chars);
  ESP_LOGI(LOG_TAG, "oh_tank_level: ADC configured and characterized (%s)",
	   (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) ? "eFuse Vref" :
	   ((val_type == ESP_ADC_CAL_VAL_EFUSE_TP) ? "Two Point" : "Default"));
  
  /* Start monitoring */
  successive_full_indications = 0;
  motor_on = false;
  beeping_now = false;
  
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
	ESP_LOGI(LOG_TAG, "oh_tank_level: Motor has started running");
	motor_on = true;
      }
    } else {
      bits = xEventGroupGetBits(mc_task_args->mc_event_group);
      if (bits & EVENT_MOTOR_RUNNING) {
	ESP_LOGD(LOG_TAG, "oh_tank_level: Motor is running, reading ADC");
	
	adc_reading = 0;
	for (i = 0; i < NO_OF_SAMPLES; i++) {
	  adc_reading += adc1_get_raw((adc1_channel_t)channel);
	}
	adc_reading /= NO_OF_SAMPLES;
	
        /* Convert adc_reading to voltage in mV */
	oh_tank_level_voltage_last = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
	ESP_LOGI(LOG_TAG, "oh_tank_level: reading raw = %lu, voltage = %lumV", adc_reading,
		 oh_tank_level_voltage_last);

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
	ESP_LOGI(LOG_TAG, "oh_tank_level: Motor has stopped running");
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
}
