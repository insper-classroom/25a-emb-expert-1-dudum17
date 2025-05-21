#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <stdio.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"

#define AUDIO_OUT_PIN 28
#define AUDIO_IN_PIN 27

#define SAMPLE_RATE 8000
#define DATA_LENGTH (SAMPLE_RATE * 4) // 4 segundos

char audio[DATA_LENGTH];
volatile int grava_pos = 0;
volatile int play_pos = 0;

SemaphoreHandle_t xSemaphorePlayInit;
SemaphoreHandle_t xSemaphorePlayDone;

int audio_pin_slice;

// PWM interrupt handler: envia um sample a cada ciclo
void pwm_interrupt_handler() {
    pwm_clear_irq(audio_pin_slice);

    if (play_pos < (DATA_LENGTH << 3)) {
        pwm_set_gpio_level(AUDIO_OUT_PIN, audio[play_pos >> 3]);
        play_pos++;
    } else {
        xSemaphoreGiveFromISR(xSemaphorePlayDone, NULL);
    }
}

// Detecta início da fala com base em limiar simples
bool detecta_fala(int amostra) {
    const int LIMIAR = 250;
    return amostra > LIMIAR;
}

void mic_task() {
    adc_gpio_init(AUDIO_IN_PIN);
    adc_init();
    adc_select_input(AUDIO_IN_PIN - 26);

    while (1) {
        printf("Esperando fala...\n");
        while (1) {
            int amostra = adc_read() / 16;
            if (detecta_fala(amostra)) break;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        printf("Gravando...\n");
        grava_pos = 0;

        int filtro_buffer[5] = {0};
        int idx = 0;
        int preenchido = 0;

        for (int i = 0; i < DATA_LENGTH; i++) {
            int nova_amostra = adc_read() / 16;

            filtro_buffer[idx] = nova_amostra;
            idx = (idx + 1) % 5;
            if (preenchido < 5) preenchido++;

            int soma = 0;
            for (int j = 0; j < preenchido; j++) {
                soma += filtro_buffer[j];
            }
            int media = soma / preenchido;

            audio[i] = (char)media;
            grava_pos++;

            sleep_us(125);  // 8000 Hz
        }

        printf("Gravação concluída.\n");
        xSemaphoreGive(xSemaphorePlayInit);

        //for (int i = 0; i < DATA_LENGTH; i++) {
            //printf("%.2f\n", 3.3 * (audio[i] / 255.0));
        //}

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
    pwm_config_set_clkdiv(&config, 8.0f);
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

    xTaskCreate(play_task, "Play Task", 4096, NULL, 1, NULL);
    xTaskCreate(mic_task, "Mic Task", 4096, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);
}
