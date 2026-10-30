/*
 * V829 PDM 麦克风 bring-up（ESP32-S3 / ESP-IDF 5.4）
 *
 * 接线（与用户提供的表一致）：
 *   V829 VDD  -> 3V3      （严禁 5V）
 *   V829 GND  -> GND
 *   V829 CLK  -> GPIO16   （PDM 时钟，由 ESP32 输出）
 *   V829 DATA -> GPIO17   （PDM 数据，麦克风输出）
 *
 * 这个固件不点灯、不跑红外，只做一件事：把 PDM 流解成 16 kHz PCM，
 * 每 100 ms 打印一次电平，并识别拍手/敲桌子这类瞬态。
 *
 * 为什么不能用板上的 MicroPython：machine.I2S 只支持标准 I2S 主模式，
 * 没有 PDM RX；PDM 需要 1 MHz 量级的时钟和抽取滤波，只能走 IDF 的 i2s_pdm 驱动。
 *
 * 构建：
 *   call C:\esp\v5.4.4\esp-idf\export.bat
 *   idf.py -p COM6 flash monitor
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---- 硬件 -----------------------------------------------------------------
#define PDM_CLK_GPIO 16
#define PDM_DIN_GPIO 17

// ---- 采样 -----------------------------------------------------------------
#define SAMPLE_RATE_HZ 16000
#define FRAME_SAMPLES 320  // 20 ms @ 16 kHz
#define FRAME_BYTES (FRAME_SAMPLES * sizeof(int16_t))
#define REPORT_EVERY_FRAMES 5  // 100 ms 打一行

// ---- 事件判定 -------------------------------------------------------------
#define CLAP_JUMP_DB 12.0      // 比慢包络高 12 dB 以上算瞬态
#define CLAP_MIN_DB (-34.0)    // 太轻的跳动忽略
#define CLAP_COOLDOWN_MS 300   // 两次拍手的最小间隔
#define DEAD_PEAK 8            // 峰值低于这个数基本等于没信号
#define DEAD_SWITCH_MS 3000    // 连续 3 秒没信号就换一个 slot 再试

static i2s_chan_handle_t s_rx = NULL;
static bool s_right_slot = false;
static bool s_tried_other_slot = false;
static uint32_t s_frames = 0;
static uint32_t s_clap_count = 0;
static int64_t s_last_clap_us = 0;
static double s_slow_db = -80.0;
static uint32_t s_dead_ms = 0;

// 把 dBFS 打印成 "-42.3" 这种形式；不用 %f，避免 newlib-nano 的格式支持差异。
static void fmt_db(double db, char *out, size_t len)
{
    int v = (int)lround(db * 10.0);
    if (v > 0) {
        v = 0;  // 0 dBFS 封顶
    }
    if (v < -999) {
        v = -999;
    }
    if (v == 0) {
        snprintf(out, len, "  0.0");
    } else {
        snprintf(out, len, "-%d.%d", (-v) / 10, (-v) % 10);
    }
}

// 0 VU 表：-70 dBFS 到 0 dBFS 映射成 24 格。
static void build_bar(double db, char *out, size_t len)
{
    int n = (int)((db + 70.0) * (double)(len - 1) / 70.0);
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n > len - 1) {
        n = (int)len - 1;
    }
    for (size_t i = 0; i + 1 < len; ++i) {
        out[i] = (i < (size_t)n) ? '#' : '.';
    }
    out[len - 1] = '\0';
}

static esp_err_t pdm_start(bool right_slot)
{
    if (s_rx != NULL) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }

    // PDM RX 只能挂在 I2S0 上；接收方向只需要 RX 句柄。
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = FRAME_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_pdm_rx_config_t cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        // PDM 模式下数据位宽固定 16 bit；单麦用 MONO + 一个 slot。
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK_GPIO,
            .din = PDM_DIN_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    // 模块的 SEL 脚决定它在时钟的哪半周期出数；接错边就是一片静音或噪声。
    cfg.slot_cfg.slot_mask = right_slot ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;

    err = i2s_channel_init_pdm_rx_mode(s_rx, &cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    return err;
}

static void analyse(const int16_t *buf, size_t n, double *out_db, int *out_peak)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += buf[i];
    }
    const double mean = sum / (double)n;

    double sq = 0.0;
    int peak = 0;
    for (size_t i = 0; i < n; ++i) {
        const double v = (double)buf[i] - mean;  // 去直流，否则 RMS 被偏置抬高
        sq += v * v;
        const int a = (int)fabs(v);
        if (a > peak) {
            peak = a;
        }
    }

    const double rms = sqrt(sq / (double)n);
    *out_db = 20.0 * log10(rms / 32768.0 + 1e-9);
    *out_peak = peak;
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    printf("\n");
    printf("=== V829 PDM mic probe ===\n");
    printf("idf     : %s\n", esp_get_idf_version());
    printf("chip    : ESP32-S3 rev v%d.%d, %d core(s)\n", chip.revision / 100, chip.revision % 100, chip.cores);
    printf("wiring  : VDD=3V3 GND=GND CLK=GPIO%d DATA=GPIO%d\n", PDM_CLK_GPIO, PDM_DIN_GPIO);
    printf("pcm     : %d Hz mono 16-bit | pdm clk = %d Hz (64 x fs)\n", SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * 64);
    printf("legend  : 0 dBFS = full scale, -70 dBFS = quiet room floor\n\n");

    esp_err_t err = pdm_start(s_right_slot);
    if (err != ESP_OK) {
        printf("!! i2s init failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("I2S PDM RX started (left slot). Speak, clap, or tap the table...\n\n");

    static int16_t buf[FRAME_SAMPLES];
    uint32_t report_in = 0;

    while (true) {
        size_t got = 0;
        err = i2s_channel_read(s_rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(200));
        if (err != ESP_OK || got < sizeof(int16_t) * 16) {
            printf("!! i2s read failed: %s (got %u bytes)\n", esp_err_to_name(err), (unsigned)got);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        const size_t n = got / sizeof(int16_t);
        s_frames++;

        if (s_frames == 1) {
            printf("first raw samples:");
            for (int i = 0; i < 8 && i < (int)n; ++i) {
                printf(" %04x", (uint16_t)buf[i]);
            }
            printf("   (all 0000 / all ffff = data line not driven)\n\n");
        }

        double db = 0.0;
        int peak = 0;
        analyse(buf, n, &db, &peak);

        // 慢包络：只跟住环境噪声，不被拍手这种 20 ms 的瞬态带跑。
        s_slow_db = s_slow_db * 0.95 + db * 0.05;

        const int64_t now_us = esp_timer_get_time();
        if (db > s_slow_db + CLAP_JUMP_DB && db > CLAP_MIN_DB &&
            (now_us - s_last_clap_us) > (int64_t)CLAP_COOLDOWN_MS * 1000) {
            s_last_clap_us = now_us;
            s_clap_count++;
            char d[8];
            fmt_db(db, d, sizeof(d));
            printf("[%u.%03us] *** event #%u: %s dBFS, %.1f dB above floor\n", (unsigned)(now_us / 1000000),
                   (unsigned)((now_us / 1000) % 1000), (unsigned)s_clap_count, d, db - s_slow_db);
        }

        // 完全没信号：先提示，再从 left slot 换到 right slot 试一次。
        if (peak < DEAD_PEAK) {
            s_dead_ms += 20;
            if (s_dead_ms >= DEAD_SWITCH_MS && !s_tried_other_slot) {
                s_tried_other_slot = true;
                s_right_slot = !s_right_slot;
                printf("\n!! no signal for %u ms (peak %d). Switching to %s slot and retrying...\n\n",
                       (unsigned)s_dead_ms, peak, s_right_slot ? "right" : "left");
                if (pdm_start(s_right_slot) != ESP_OK) {
                    printf("!! re-init failed\n");
                }
                s_dead_ms = 0;
                s_slow_db = -80.0;
                continue;
            }
        } else {
            s_dead_ms = 0;
        }

        if (++report_in >= REPORT_EVERY_FRAMES) {
            report_in = 0;
            char d[8];
            char bar[25];
            fmt_db(db, d, sizeof(d));
            build_bar(db, bar, sizeof(bar));
            printf("[%u.%03us] %s dBFS |%s| pk %5d%s\n", (unsigned)(now_us / 1000000),
                   (unsigned)((now_us / 1000) % 1000), d, bar, peak, s_dead_ms > 1000 ? "  (no signal)" : "");
        }
    }
}
