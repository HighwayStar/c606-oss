/* Backlight PWM, mirrors vendor MidLcdPwmInit(): LEDC low-speed timer 0,
 * 10-bit, 20 kHz on GPIO45, channel 0, active high. */
#include "driver/ledc.h"
#include "esp_check.h"
#include "board.h"
#include "backlight.h"

static const char *TAG = "bl";

esp_err_t backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_PWM_RES_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "timer");

    ledc_channel_config_t c = {
        .gpio_num = BL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&c), TAG, "channel");
    return ESP_OK;
}

void backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = ((1u << BL_PWM_RES_BITS) - 1) * percent / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}
