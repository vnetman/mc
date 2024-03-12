#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "pins.h"
#include "mc.h"

static char const *LOG_TAG = "mc|motor";

void motor_task (void *param) {
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;
  bool motor_running = false;
  bool desired_state = false;
  
  /* We keep track of the current value of the output gpio level, because
     turning the motor on/off is a matter of toggling this value.
     In gpio.c, the MOTOR_OUT GPIO pin is set to 1 during bootup. */
  uint32_t current_motor_out_gpio_level = 1;

  /* loop, waiting for enqueues to the queue, and obey.
     Monitor the gpio input and set the bit in the event group */
  while (pdTRUE) {
    /* Read the Input GPIO to see if the motor is running */
    if (gpio_get_level(MOTOR_RUNNING_SENSE_IN) == 0) {
      if (motor_running) {
	motor_running = false;
	xEventGroupClearBits(mc_task_args->mc_event_group, EVENT_MOTOR_RUNNING);
      } else {
	/* Motor is not running, no change in state */
      }
    } else {
      if (!motor_running) {
	motor_running = true;
	xEventGroupSetBits(mc_task_args->mc_event_group, EVENT_MOTOR_RUNNING);
      } else {
	/* Motor is running, no change in state */
      }      
    }
    
    if (pdTRUE == xQueueReceive(mc_task_args->motor_on_off_q, (void *) &desired_state,
				pdMS_TO_TICKS(1000))) {
      ESP_LOGI(LOG_TAG, "rx request on q, desired_state = %s", desired_state ? "on" : "off");
      if (desired_state == motor_running) {
	ESP_LOGI(LOG_TAG, "Ignoring request because desired state "
		 "(%s) == motor running state", desired_state ? "on" : "off");
      } else {
	ESP_LOGI(LOG_TAG, "Obeying request because desired state "
		 "(%s) != motor running state (%s)",
		 desired_state ? "on" : "off", motor_running ? "on" : "off");
	
	/* Toggle the relay state */
	if (current_motor_out_gpio_level == 1) {
	  current_motor_out_gpio_level = 0;
	} else {
	  current_motor_out_gpio_level = 1;
	}
	gpio_set_level(MOTOR_OUT, current_motor_out_gpio_level);
      }
    } else {
      /* Nothing was enqueued, go back to the beginning of the loop to read the
	 running state of the motor */
    }
  }
}
