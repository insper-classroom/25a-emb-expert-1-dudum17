#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>

#include <math.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/adc.h"

#define AUDIO_OUT_PIN 28
#define AUDIO_IN_PIN 27

#define SAMPLE_RATE 8000
#define DATA_LENGTH (SAMPLE_RATE * 4)  // 4 segundos

char audio[DATA_LENGTH];

volatile int grava_pos = 0;
volatile int play_pos = 0;

SemaphoreHandle_t xSemaphorePlayInit;
SemaphoreHandle_t xSemaphorePlayDone;
SemaphoreHandle_t xSemaphoreRecordDone;

int audio_pin_slice;

void pwm_interrupt_handler() {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_OUT_PIN));
    if (play_pos < (DATA_LENGTH << 3)) {
        pwm_set_gpio_level(AUDIO_OUT_PIN, audio[play_pos >> 3]);
        play_pos++;
    } else {
        xSemaphoreGiveFromISR(xSemaphorePlayDone, 0);
    }
}

bool detecta_fala(int amostra) {
    const int LIMIAR = 70;
    return amostra > LIMIAR;
}

bool timer_0_callback(repeating_timer_t* rt) {
    static int buffer[5] = {0};
    static int idx = 0;

    if (grava_pos < DATA_LENGTH) {
        buffer[idx] = adc_read() / 16;
        idx = (idx + 1) % 5;

        int soma = 0;
        for (int i = 0; i < 5; i++) soma += buffer[i];
        int media = soma / 5;

        audio[grava_pos++] = (char)media;
        return true;
    } else {
        xSemaphoreGiveFromISR(xSemaphoreRecordDone, 0);
        return false;
    }
}

void mic_task() {
    adc_gpio_init(AUDIO_IN_PIN);
    adc_init();
    adc_select_input(AUDIO_IN_PIN - 26);

    int timer_0_hz = SAMPLE_RATE;
    repeating_timer_t timer_0;

    while (1) {
        printf("Esperando fala...\n");
        while (1) {
            int amostra = adc_read() / 16;
            if (detecta_fala(amostra)) break;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        printf("Gravando...\n");
        grava_pos = 0;

        if (!add_repeating_timer_us(1000000 / timer_0_hz, timer_0_callback, NULL, &timer_0)) {
            printf("Erro ao iniciar o timer\n");
        }

        if (xSemaphoreTake(xSemaphoreRecordDone, portMAX_DELAY) == pdTRUE) {
            printf("Gravação concluída.\n");
        }

        xSemaphoreGive(xSemaphorePlayInit);

        for (int i = 0; i < DATA_LENGTH; i++) {
            printf("%.2f\n", 3.3 * (audio[i] / 255.0));
        }

        if (xSemaphoreTake(xSemaphorePlayDone, portMAX_DELAY) == pdTRUE) {
            pwm_set_enabled(audio_pin_slice, false);
        }
    }
}

void play_task() {
    gpio_set_function(AUDIO_OUT_PIN, GPIO_FUNC_PWM);
    audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_OUT_PIN);

    pwm_clear_irq(audio_pin_slice);
    pwm_set_irq_enabled(audio_pin_slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 5.45f);
    pwm_config_set_wrap(&config, 250);

    while (1) {
        if (xSemaphoreTake(xSemaphorePlayInit, portMAX_DELAY) == pdTRUE) {
            play_pos = 0;
            pwm_init(audio_pin_slice, &config, true);
            pwm_set_gpio_level(AUDIO_OUT_PIN, 0);
        }
    }
}

int main() {
    stdio_init_all();
    printf("Sistema iniciado\n");

    xSemaphorePlayInit = xSemaphoreCreateBinary();
    xSemaphorePlayDone = xSemaphoreCreateBinary();
    xSemaphoreRecordDone = xSemaphoreCreateBinary();

    xTaskCreate(play_task, "Play Task", 4096, NULL, 1, NULL);
    xTaskCreate(mic_task, "Mic Task", 4096, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);
}
