/*
 * INMP441 麦克风 bring-up（ESP32-S3 / ESP-IDF 5.4）
 *
 * 接线（标准 I2S，四根线一根都不能少）：
 *   INMP441 VDD -> 3V3     （严禁 5V）
 *   INMP441 GND -> GND
 *   INMP441 L/R -> GND     （接地 = 左声道）
 *   INMP441 SCK -> GPIO16  （BCLK 位时钟，由 ESP32 输出）
 *   INMP441 WS  -> GPIO18  （LRCLK 声道时钟，由 ESP32 输出）
 *   INMP441 SD  -> GPIO17  （数据，麦克风输出）
 *
 * 这个固件不点灯、不跑红外，只做一件事：把 I2S 流解成 16 kHz PCM，
 * 每 100 ms 打印一次电平，并识别拍手/敲桌子这类瞬态。
 *
 * INMP441 输出 24 bit、在 32 bit 槽里左对齐，且要求 WS 周期 64 BCLK。
 * 用 16 bit 槽只有 32 BCLK，数据会错位成低频漂移，所以这里固定 32 bit 槽，
 * 读到 int32 后右移 16 位取高 16 bit，再乘 MIC_GAIN。
 *
 * 目录名 pdm_probe 是历史遗留（上一版接的是 PDM 麦克风），现在探的是标准 I2S。
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

#include "driver/i2s_std.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---- 硬件 -----------------------------------------------------------------
#define MIC_SCK_GPIO 16  // BCLK
#define MIC_WS_GPIO 18   // LRCLK
#define MIC_SD_GPIO 17   // 数据（麦克风 -> ESP32）
#define MIC_GAIN 4       // 24 bit 右移后只有几十 LSB，先放大再判电平

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

static esp_err_t mic_start(bool right_slot)
{
    if (s_rx != NULL) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }

    // 接收方向只需要 RX 句柄；I2S0 做主模式，自己产生 BCLK/WS。
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = FRAME_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        // 32 bit 槽 + MONO：每个采样占左右两个 32 bit 槽，只取其中一边。
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    // L/R 接 GND = 左声道，接 3V3 = 右声道；接错边就是一片静音。
    cfg.slot_cfg.slot_mask = right_slot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_rx, &cfg);
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

// 读一帧并转成 int16：32 bit 槽 -> 右移 16 位 -> 乘增益 -> 限幅。
static size_t read_frame(int16_t *out, size_t max_samples)
{
    static int32_t raw[FRAME_SAMPLES];
    size_t got = 0;
    if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(200)) != ESP_OK) {
        return 0;
    }
    size_t n = got / sizeof(int32_t);
    if (n > max_samples) {
        n = max_samples;
    }
    for (size_t i = 0; i < n; ++i) {
        int32_t v = (raw[i] >> 16) * MIC_GAIN;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        out[i] = (int16_t)v;
    }
    return n;
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
    printf("=== INMP441 I2S mic probe ===\n");
    printf("idf     : %s\n", esp_get_idf_version());
    printf("chip    : ESP32-S3 rev v%d.%d, %d core(s)\n", chip.revision / 100, chip.revision % 100, chip.cores);
    printf("wiring  : VDD=3V3 GND=GND L/R=GND SCK=GPIO%d WS=GPIO%d SD=GPIO%d\n", MIC_SCK_GPIO, MIC_WS_GPIO,
           MIC_SD_GPIO);
    printf("pcm     : %d Hz mono 16-bit | bclk = %d Hz (32 bit x 2 ch)\n", SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * 64);
    printf("legend  : 0 dBFS = full scale, -70 dBFS = quiet room floor\n\n");

    esp_err_t err = mic_start(s_right_slot);
    if (err != ESP_OK) {
        printf("!! i2s init failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("I2S std RX started (left slot). Speak, clap, or tap the table...\n\n");

    static int16_t buf[FRAME_SAMPLES];
    uint32_t report_in = 0;

    while (true) {
        const size_t n = read_frame(buf, FRAME_SAMPLES);
        if (n < 16) {
            printf("!! i2s read returned %u samples\n", (unsigned)n);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

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
                if (mic_start(s_right_slot) != ESP_OK) {
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
