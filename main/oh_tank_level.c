#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
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

#define ERRBUFLEN 32
static char errbuf[ERRBUFLEN];

#define OH_TANK_LEVEL_ADC_UNIT  ADC_UNIT_1
#define OH_TANK_LEVEL_ADC_ATTEN ADC_ATTEN_DB_0

/* This is a global because the HTTP server reads it */
uint32_t oh_tank_level_voltage_last = 0;

static char const *LOG_TAG = "mc|oh_tank_level";

static void oh_tank_level_read_adc (adc_oneshot_unit_handle_t adc_handle,
				    adc_cali_handle_t adc_cali_handle,
				    bool calibration_ok) {
  static int adc_raw[128];
  static int adc_voltage[128];
  int mini, maxi, avrg, samples;
  esp_err_t err;
  unsigned int i;

  memset(&adc_raw[0], 0, sizeof(adc_raw));
  memset(&adc_voltage[0], 0, sizeof(adc_voltage));

  samples = sizeof(adc_raw)/sizeof(adc_raw[0]);
  
  for (i = 0; i < samples; i++) {
    err = adc_oneshot_read(adc_handle, WATER_LEVEL_ADC_CHANNEL, &adc_raw[i]);
    if (err == ESP_OK) {
      if (calibration_ok) {
	err = adc_cali_raw_to_voltage(adc_cali_handle, adc_raw[i], &adc_voltage[i]);
	if (err != ESP_OK) {
	  ESP_LOGE(LOG_TAG, "adc_cali_raw_to_voltage() failed: %s",
		   esp_err_to_name_r(err, errbuf, ERRBUFLEN));
	  break;
	}
      } else {
	ESP_LOGE(LOG_TAG, "Cannot get voltages, calibration_ok == false");
	break;
      }
    } else {
      ESP_LOGE(LOG_TAG, "adc_oneshot_read() failed: %s", esp_err_to_name_r(err, errbuf, ERRBUFLEN));
      break;
    }
  }

  if (i != samples) {
    return;
  }

  mini = 4095;
  maxi = 0;
  avrg = 0;
  for (i = 0; i < samples; i++) {
    if (adc_raw[i] > maxi) {
      maxi = adc_raw[i];
    }
    if (adc_raw[i] < mini) {
      mini = adc_raw[i];
    }
    avrg += adc_raw[i];
  }
  avrg /= samples;

  ESP_LOGI(LOG_TAG, "avrg = %d", avrg);
}

static bool oh_tank_level_adc_calibration_init (adc_cali_handle_t *out_handle) {
  adc_cali_handle_t handle = NULL;
  esp_err_t ret = ESP_FAIL;
  bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  if (!calibrated) {
    ESP_LOGI(LOG_TAG, "calibration scheme version is %s", "Curve Fitting");
    adc_cali_curve_fitting_config_t cali_config = {
      .unit_id = OH_TANK_LEVEL_ADC_UNIT,
      .chan = WATER_LEVEL_ADC_CHANNEL,
      .atten = OH_TANK_LEVEL_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
    if (ret == ESP_OK) {
      calibrated = true;
    } else {
      ESP_LOGE(LOG_TAG, "adc_cali_create_scheme_curve_fitting() failed: %s",
	       esp_err_to_name_r(err, errbuf, ERRBUFLEN));
    }
  }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  if (!calibrated) {
    ESP_LOGI(LOG_TAG, "calibration scheme version is %s", "Line Fitting");
    adc_cali_line_fitting_config_t cali_config = {
      .unit_id = OH_TANK_LEVEL_ADC_UNIT,
      .atten = OH_TANK_LEVEL_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
    if (ret == ESP_OK) {
      calibrated = true;
    } else {
      ESP_LOGE(LOG_TAG, "adc_cali_create_scheme_line_fitting() failed: %s",
	       esp_err_to_name_r(ret, errbuf, ERRBUFLEN));
    }
  }
#endif

  *out_handle = handle;
  if (ret == ESP_OK) {
    ESP_LOGI(LOG_TAG, "ADC calibration success");
  } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
    ESP_LOGW(LOG_TAG, "eFuse not burnt, skip software calibration");
  } else {
  }

  return calibrated;
}

static bool oh_tank_level_adc_init (adc_oneshot_unit_handle_t *ptr_adc_handle,
				    adc_cali_handle_t *ptr_adc_cali_handle,
				    bool *ptr_calibration_ok) {
  esp_err_t err;

  adc_oneshot_unit_init_cfg_t init_config = {
    .unit_id = OH_TANK_LEVEL_ADC_UNIT,
  };
  
  err = adc_oneshot_new_unit(&init_config, ptr_adc_handle);
  if (err == ESP_OK) {
    adc_oneshot_chan_cfg_t config = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = OH_TANK_LEVEL_ADC_ATTEN,
    };
    
    err = adc_oneshot_config_channel(*ptr_adc_handle, WATER_LEVEL_ADC_CHANNEL, &config);
    if (err == ESP_OK) {
      ESP_LOGI(LOG_TAG, "adc initialized ok");
      *ptr_calibration_ok = oh_tank_level_adc_calibration_init(ptr_adc_cali_handle);
      return true;
    } else {
      ESP_LOGE(LOG_TAG, "adc_oneshot_config_channel() failed: %s",
	       esp_err_to_name_r(err, errbuf, ERRBUFLEN));
      return false;
    }
  } else {
    ESP_LOGE(LOG_TAG, "adc_oneshot_new_unit() failed: %s",
	     esp_err_to_name_r(err, errbuf, ERRBUFLEN));
    return false;
  }
}

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
  adc_oneshot_unit_handle_t adc_handle;
  adc_cali_handle_t adc_cali_handle;
  bool adc_calibration_ok, adc_configured, adc_ok;
  EventBits_t bits;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;

  adc_configured = false;
  adc_ok = false;

  while (pdTRUE) {
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
    
    bits = xEventGroupWaitBits(mc_task_args->mc_event_group,
			       EVENT_MOTOR_RUNNING,
			       pdFALSE /* don't clear bit on exit */,
			       pdFALSE /* xWaitForAllBits */,
			       pdMS_TO_TICKS(1000));
    if (bits & EVENT_MOTOR_RUNNING) {
      ESP_LOGI(LOG_TAG, "Motor is running");
      vTaskDelay(pdMS_TO_TICKS(1000));
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
