/**
 * ============================================================================
 * CONTROLE TRAFFIC RIDER  -  v1 + SMP (multicore)
 * ----------------------------------------------------------------------------
 * Raspberry Pi Pico 2 (RP2350) + FreeRTOS em modo SMP (2 cores).
 * MPU6050 (I2C) + 4 botoes (IRQ) + LED RGB (status) + USB-CDC (datagrama).
 *
 * Estrutura RTOS (sem variaveis globais de estado; toda comunicacao por
 * fila/semaforo):
 *
 *   MPU6050 --I2C--> imu_task --xQueueMPU--> fusion_task --xQueueSteer--\
 *                                                                        +-> uart_task --USB--> PC
 *   4 botoes --IRQ--> btn_isr --xSemBtn--> button_task --xQueueButtons--/
 *
 *   stdio_usb_connected() --(polling)--> led_task --PWM--> LED RGB
 *
 * SMP - afinidade de core (igual a logica do lab de RTOS):
 *   Core 0: imu, button, uart, led   -> pipeline de I/O, baixa latencia
 *   Core 1: fusion                   -> AHRS/FPU pesado, core dedicado
 *   (quando a ai_task do Edge Impulse entrar, ela vai para o Core 1 tambem.)
 *
 * Requer no FreeRTOSConfig.h (voce ja tem, pois o lab de SMP compila):
 *   configNUMBER_OF_CORES        = 2   (vem do CMakeLists)
 *   configUSE_CORE_AFFINITY      = 1
 *   configRUN_MULTIPLE_PRIORITIES= 1   (essencial: deixa fusion rodar no Core 1
 *                                       em paralelo com tasks de prio diferente
 *                                       no Core 0)
 * ============================================================================
 */

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/time.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "mpu6050.h"

#include "Fusion.h"

// ============================================================================
// HARDWARE
// ============================================================================
#define MPU_ADDRESS    0x68
#define I2C_SDA_GPIO   4
#define I2C_SCL_GPIO   5
#define I2C_BAUD       (400 * 1000)

#define LED_R_PIN      16
#define LED_G_PIN      17
#define LED_B_PIN      18
#define PWM_WRAP       255
#define COMMON_ANODE   0     // 1 = LED RGB de anodo comum (inverte os niveis)

// Botoes: ativos em nivel BAIXO (pull-up interno). Pressionado => gpio_get==0.
#define BTN_ACCEL_PIN  6
#define BTN_BRAKE_PIN  7
#define BTN_HORN_PIN   8
#define BTN_PAUSE_PIN  9

// Pinos de instrumentacao para o Saleae (lab RTOS).
#define IMU_TASK_PIN     10
#define FUSION_TASK_PIN  11
#define LED_TASK_PIN     12
#define UART_TASK_PIN    13

// ============================================================================
// INSTRUMENTACAO DE STACK
//   0 = build "joga" (stream limpo, SEM printf). USE ESTE para jogar.
//   1 = imprime high-water-mark das stacks (so para o lab). O printf SUJA o
//       stream do datagrama, entao o jogo nao funciona com isto ligado.
//       Requer configUSE_TRACE_FACILITY=1 e INCLUDE_uxTaskGetStackHighWaterMark=1.
// ============================================================================
#define ENABLE_STACK_MONITOR 0

// ============================================================================
// SMP / AFINIDADE
// ============================================================================
#define CORE_0 (1 << 0)
#define CORE_1 (1 << 1)

// ============================================================================
// PROTOCOLO  (datagrama de 4 bytes - formato SYNC-first, igual ao seu main.py)
//   ordem na serial:  SYNC(0xFF)  AXIS  VAL_0(LSB)  VAL_1(MSB)
//   VAL = inteiro de 16 bits COM sinal (little-endian, complemento de dois).
// ============================================================================
#define SYNC_BYTE      0xFF
#define AXIS_STEER     0     // canal 0: estercamento (-255..+255)
#define AXIS_BUTTONS   1     // canal 1: bitmask de botoes/gestos

// Bits do canal 1 (devem casar com o script Python).
#define BIT_ACCEL    0   // -> tecla UP
#define BIT_BRAKE    1   // -> tecla DOWN
#define BIT_HORN     2   // -> tecla H
#define BIT_PAUSE    3   // -> tecla P
#define BIT_WHEELIE  4   // -> tecla Y (reservado p/ gesto via IA - fase futura)

// ============================================================================
// ESTERCAMENTO (roll do IMU)
// ============================================================================
#define STEER_MAX_DEG    40.0f
#define STEER_LIMIT      255
#define STEER_DEADZONE   8
#define STEER_INVERT     0     // 1 inverte o sentido se virar pro lado errado

// ============================================================================
// PERIODOS (multiplos de 10ms; tick = 100 Hz)
// ============================================================================
#define IMU_PERIOD_MS    10    // 100 Hz
#define UART_PERIOD_MS   20    // 50 Hz
#define LED_PERIOD_MS    100   // 10 Hz
#define DEBOUNCE_MS      20

// ============================================================================
// TIPOS
// ============================================================================
typedef struct mpu_data {
    int16_t  accel[3];
    int16_t  gyro[3];
    uint64_t timestamp_us;
} mpu_data_t;

// ============================================================================
// HANDLES RTOS (escopo de arquivo - sao handles, nao "estado global")
// ============================================================================
static QueueHandle_t     xQueueMPU;       // imu_task    -> fusion_task
static QueueHandle_t     xQueueSteer;     // fusion_task  -> uart_task  (mailbox)
static QueueHandle_t     xQueueButtons;   // button_task  -> uart_task  (mailbox)
static SemaphoreHandle_t xSemaphoreBtn;   // ISR          -> button_task

// ============================================================================
// HELPERS
// ============================================================================
static inline void instrument_pin_init(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
}

static void mpu6050_init(void) {
    i2c_init(i2c_default, I2C_BAUD);
    gpio_set_function(I2C_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_GPIO);
    gpio_pull_up(I2C_SCL_GPIO);

    uint8_t buf[] = {MPUREG_PWR_MGMT_1, 0x00};  // tira do sleep
    i2c_write_blocking(i2c_default, MPU_ADDRESS, buf, 2, false);
}

static void mpu6050_read_raw(int16_t accel[3], int16_t gyro[3], int16_t *temp) {
    uint8_t buffer[14];
    uint8_t reg = MPUREG_ACCEL_XOUT_H;  // 0x3B

    i2c_write_blocking(i2c_default, MPU_ADDRESS, &reg, 1, true);
    i2c_read_blocking(i2c_default, MPU_ADDRESS, buffer, 14, false);

    for (int i = 0; i < 3; i++)
        accel[i] = (int16_t)((buffer[i * 2] << 8) | buffer[(i * 2) + 1]);
    *temp = (int16_t)((buffer[6] << 8) | buffer[7]);
    for (int i = 0; i < 3; i++)
        gyro[i] = (int16_t)((buffer[8 + i * 2] << 8) | buffer[8 + (i * 2) + 1]);
}

static void pwm_setup_pin(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_set_wrap(slice, PWM_WRAP);
    pwm_set_gpio_level(pin, 0);
    pwm_set_enabled(slice, true);
}

static void rgb_set(uint8_t r, uint8_t g, uint8_t b) {
#if COMMON_ANODE
    r = PWM_WRAP - r; g = PWM_WRAP - g; b = PWM_WRAP - b;
#endif
    pwm_set_gpio_level(LED_R_PIN, r);
    pwm_set_gpio_level(LED_G_PIN, g);
    pwm_set_gpio_level(LED_B_PIN, b);
}

// 1 datagrama: SYNC, AXIS, LSB, MSB  (mesmo formato do main.py do mouse).
static void send_datagram(uint8_t axis, int16_t value) {
    putchar_raw(SYNC_BYTE);
    putchar_raw(axis);
    putchar_raw((uint8_t)(value & 0xFF));          // VAL_0 (LSB)
    putchar_raw((uint8_t)((value >> 8) & 0xFF));   // VAL_1 (MSB)
}

// ============================================================================
// ISR DOS BOTOES (uma callback p/ os 4 GPIOs; so acorda a task)
// ============================================================================
static void btn_isr(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(xSemaphoreBtn, &hpw);
    portYIELD_FROM_ISR(hpw);
}

// ============================================================================
// TASKS
// ============================================================================

void imu_task(void *p) {
    (void)p;
    instrument_pin_init(IMU_TASK_PIN);
    mpu6050_init();

    TickType_t xLastWake = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(IMU_PERIOD_MS);

    while (true) {
        gpio_put(IMU_TASK_PIN, 1);

        mpu_data_t data;
        int16_t temp;
        mpu6050_read_raw(data.accel, data.gyro, &temp);
        data.timestamp_us = time_us_64();
        xQueueSend(xQueueMPU, &data, 0);

        gpio_put(IMU_TASK_PIN, 0);
        vTaskDelayUntil(&xLastWake, xPeriod);
    }
}

void fusion_task(void *p) {
    (void)p;
    instrument_pin_init(FUSION_TASK_PIN);

    FusionAhrs ahrs;
    FusionAhrsInitialise(&ahrs);

    mpu_data_t data;
    uint64_t last_ts_us = 0;
    bool first_dt = true;

    while (true) {
        if (xQueueReceive(xQueueMPU, &data, portMAX_DELAY)) {
            gpio_put(FUSION_TASK_PIN, 1);

            float dt;
            if (first_dt) { dt = (float)IMU_PERIOD_MS / 1000.0f; first_dt = false; }
            else          { dt = (float)(data.timestamp_us - last_ts_us) / 1e6f; }
            last_ts_us = data.timestamp_us;

            FusionVector gyro = {
                .axis.x = data.gyro[0] / 131.0f,
                .axis.y = data.gyro[1] / 131.0f,
                .axis.z = data.gyro[2] / 131.0f,
            };
            FusionVector accel = {
                .axis.x = data.accel[0] / 16384.0f,
                .axis.y = data.accel[1] / 16384.0f,
                .axis.z = data.accel[2] / 16384.0f,
            };

            FusionAhrsUpdateNoMagnetometer(&ahrs, gyro, accel, dt);
            FusionEuler e = FusionQuaternionToEuler(FusionAhrsGetQuaternion(&ahrs));

            float roll = e.angle.roll;
            int32_t steer = (int32_t)(roll / STEER_MAX_DEG * STEER_LIMIT);
#if STEER_INVERT
            steer = -steer;
#endif
            if (steer >  STEER_LIMIT) steer =  STEER_LIMIT;
            if (steer < -STEER_LIMIT) steer = -STEER_LIMIT;
            if (steer > -STEER_DEADZONE && steer < STEER_DEADZONE) steer = 0;

            int16_t steer16 = (int16_t)steer;
            xQueueOverwrite(xQueueSteer, &steer16);

            gpio_put(FUSION_TASK_PIN, 0);
        }
    }
}

void button_task(void *p) {
    (void)p;
    uint16_t last_mask = 0xFFFF;  // forca o 1o envio

    while (true) {
        if (xSemaphoreTake(xSemaphoreBtn, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            uint16_t mask = 0;
            if (!gpio_get(BTN_ACCEL_PIN)) mask |= (1u << BIT_ACCEL);
            if (!gpio_get(BTN_BRAKE_PIN)) mask |= (1u << BIT_BRAKE);
            if (!gpio_get(BTN_HORN_PIN))  mask |= (1u << BIT_HORN);
            if (!gpio_get(BTN_PAUSE_PIN)) mask |= (1u << BIT_PAUSE);

            if (mask != last_mask) {
                last_mask = mask;
                xQueueOverwrite(xQueueButtons, &mask);
            }
        }
    }
}

void uart_task(void *p) {
    (void)p;
    instrument_pin_init(UART_TASK_PIN);

    TickType_t xLastWake = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(UART_PERIOD_MS);

    int16_t  steer = 0;
    uint16_t btns  = 0;

    while (true) {
        gpio_put(UART_TASK_PIN, 1);

        xQueuePeek(xQueueSteer,   &steer, 0);
        xQueuePeek(xQueueButtons, &btns,  0);

        send_datagram(AXIS_STEER,   steer);
        send_datagram(AXIS_BUTTONS, (int16_t)btns);

        gpio_put(UART_TASK_PIN, 0);
        vTaskDelayUntil(&xLastWake, xPeriod);
    }
}

void led_task(void *p) {
    (void)p;
    instrument_pin_init(LED_TASK_PIN);
    pwm_setup_pin(LED_R_PIN);
    pwm_setup_pin(LED_G_PIN);
    pwm_setup_pin(LED_B_PIN);

    const TickType_t xPeriod = pdMS_TO_TICKS(LED_PERIOD_MS);

    while (true) {
        gpio_put(LED_TASK_PIN, 1);

        if (stdio_usb_connected()) rgb_set(0, 60, 0);   // verde = conectado
        else                       rgb_set(60, 0, 0);   // vermelho = sem PC

        gpio_put(LED_TASK_PIN, 0);
        vTaskDelay(xPeriod);
    }
}

#if ENABLE_STACK_MONITOR
void stack_monitor_task(void *p) {
    (void)p;
    static TaskStatus_t tasks[16];
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        UBaseType_t n = uxTaskGetSystemState(tasks, 16, NULL);
        printf("+------------------+-------+\n");
        printf("| %-16s | %5s |\n", "task", "free");
        printf("+------------------+-------+\n");
        for (UBaseType_t i = 0; i < n; i++)
            printf("| %-16s | %5u |\n", tasks[i].pcTaskName,
                   (unsigned)tasks[i].usStackHighWaterMark);
        printf("+------------------+-------+\n");
        printf("| heap livre min   | %5u |\n",
               (unsigned)xPortGetMinimumEverFreeHeapSize());
        printf("+------------------+-------+\n\n");
    }
}
#endif

// ============================================================================
// MAIN
// ============================================================================
int main() {
    stdio_init_all();

    // Botoes: entrada com pull-up + IRQ nas duas bordas (press e release).
    const uint btn_pins[] = {BTN_ACCEL_PIN, BTN_BRAKE_PIN, BTN_HORN_PIN, BTN_PAUSE_PIN};
    for (int i = 0; i < 4; i++) {
        gpio_init(btn_pins[i]);
        gpio_set_dir(btn_pins[i], GPIO_IN);
        gpio_pull_up(btn_pins[i]);
    }
    gpio_set_irq_enabled_with_callback(btn_pins[0],
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &btn_isr);
    for (int i = 1; i < 4; i++)
        gpio_set_irq_enabled(btn_pins[i],
            GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);

    // Filas e semaforo.
    xQueueMPU     = xQueueCreate(16, sizeof(mpu_data_t));
    xQueueSteer   = xQueueCreate(1,  sizeof(int16_t));    // mailbox
    xQueueButtons = xQueueCreate(1,  sizeof(uint16_t));   // mailbox
    xSemaphoreBtn = xSemaphoreCreateBinary();

    // Tasks.
    TaskHandle_t hImu, hFusion, hButton, hUart, hLed;
    xTaskCreate(imu_task,    "imu",    256, NULL, 3, &hImu);
    xTaskCreate(fusion_task, "fusion", 512, NULL, 2, &hFusion);
    xTaskCreate(button_task, "button", 256, NULL, 3, &hButton);
    xTaskCreate(uart_task,   "uart",   256, NULL, 3, &hUart);
    xTaskCreate(led_task,    "led",    256, NULL, 1, &hLed);
#if ENABLE_STACK_MONITOR
    TaskHandle_t hMonitor;
    xTaskCreate(stack_monitor_task, "monitor", 512, NULL, 1, &hMonitor);
#endif

    // ---- SMP: afinidade de core (igual ao lab de RTOS) ----
    // Core 0 = pipeline de I/O; Core 1 = fusion (FPU pesado, dedicado).
    vTaskCoreAffinitySet(hImu,    CORE_0);
    vTaskCoreAffinitySet(hButton, CORE_0);
    vTaskCoreAffinitySet(hUart,   CORE_0);
    vTaskCoreAffinitySet(hLed,    CORE_0);
    vTaskCoreAffinitySet(hFusion, CORE_1);
#if ENABLE_STACK_MONITOR
    vTaskCoreAffinitySet(hMonitor, CORE_0);
#endif

    vTaskStartScheduler();

    while (true) { ; }
    return 0;
}