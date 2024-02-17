#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "pins.h"
#include "mc.h"

static void make_some_noise (void) {
  BaseType_t i;
  for (i = 0; i < 4; i++) {
    gpio_set_level(BEEP_OUT, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(BEEP_OUT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void beep_task (void *param) {
  bool current_state, desired_state;
  QueueHandle_t beep_q = ((struct mc_task_args_t_ *) param)->beep_q;
  
  current_state = false;

  /* Loop forever, looking for enqueues to the beep_q */
  while (1) {
    desired_state = false;
    if (pdTRUE == xQueueReceive(beep_q, (void *) &desired_state, 
				pdMS_TO_TICKS(1000))) {
      /* Something was enqueued */
      if (desired_state == current_state) {
	/* Nothing to do */
	ESP_LOGI("mc", "beep_task: desired_state is identical to current_state");
	continue;
      }
      if (desired_state == true) {
	/* turn beep on */
	ESP_LOGI("mc", "beep_task: setting beep on");
	make_some_noise();
      } else {
	/* turn beep off */
	ESP_LOGI("mc", "beep_task: setting beep off");
	gpio_set_level(BEEP_OUT, 0);
      }
      current_state = desired_state;
    } else {
      /* Timed out without receiving anything on the queue */
      if (current_state == true) {
	make_some_noise();
      }
    }
  }
}
