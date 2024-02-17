#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "pins.h"
#include "mc.h"

void motor_task (void *param) {
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;
  bool motor_running = false;
  bool desired_state = false;

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
      ESP_LOGD("mc", "motor_task: received request on q, desired_state = %s",
	       desired_state ? "on" : "off");
      if (desired_state == motor_running) {
	ESP_LOGI("mc", "motor_task: Ignoring request received on q because desired state "
		 "(%s) is the same as the motor running state", desired_state ? "on" : "off");
      } else {
	ESP_LOGI("mc", "motor_task: Obeying request received on q because desired state "
		 "(%s) is different from the motor running state (%s)",
		 desired_state ? "on" : "off", motor_running ? "on" : "off");
	gpio_set_level(MOTOR_OUT, desired_state ? 1 : 0);
      }
    } else {
      /* Nothing was enqueued, go back to the beginning of the loop to read the
	 running state of the motor */
    }
  }
}
