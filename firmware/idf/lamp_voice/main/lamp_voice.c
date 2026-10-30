/*
 * LAMP-1 语音客户端（ESP32-S3 + INMP441 I2S 麦克风）
 *
 * 做的事：
 *   1. 从 I2S 麦克风连续采集 16 kHz / 16 bit 单声道 PCM
 *   2. 用能量做端点检测（VAD）：检测到说话就开一段，静音 800 ms 收尾
 *   3. 边说边通过 WiFi 把 PCM 流给 PC 上的 lamp_server.py（TCP 8766）
 *   4. 把 PC 回来的 HEARD / REPLY 文字逐行打印到串口
 *
 * 麦克风接线（标准 I2S，四根线一根都不能少）：
 *   VDD -> 3V3     GND -> GND     L/R -> GND（左声道）
 *   SCK -> GPIO16（BCLK）  WS -> GPIO18（LRCLK）  SD -> GPIO17（数据）
 *
 * 串口协议（PC 侧 server/lamp_server.py 实现另一端）：
 *   PCM <字节数>\n + <PCM 字节>
 *   END\n
 *   回：HEARD ... / REPLY ... / DONE / ERR ...
 *
 * 没配 WiFi 时：启动后扫描并打印附近 SSID，方便挑一个填进 menuconfig。
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "driver/i2s_std.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

// ---- 硬件 -----------------------------------------------------------------
#define MIC_SCK_GPIO 16  // BCLK
#define MIC_WS_GPIO 18   // LRCLK
#define MIC_SD_GPIO 17   // 数据（麦克风 -> ESP32）
#define MIC_GAIN 4       // 24 bit 右移后只有几十 LSB，先放大再算 dBFS

// ---- 音频 -----------------------------------------------------------------
#define SAMPLE_RATE_HZ 16000
#define FRAME_SAMPLES 320  // 20 ms
#define FRAME_BYTES (FRAME_SAMPLES * sizeof(int16_t))
#define PREROLL_FRAMES 5  // 起手前留 100 ms，别把第一个字切掉

// ---- VAD ------------------------------------------------------------------
#define VAD_START_DB (-45.0)  // 高于这个电平才可能是人声
#define VAD_START_FRAMES 2    // 连续 2 帧（40 ms）才算开口
#define VAD_END_DB (-50.0)    // 低于这个电平算静音
#define VAD_END_FRAMES 40     // 连续 800 ms 静音收尾
#define VAD_MIN_FRAMES 10     // 太短（200 ms）当噪声丢掉
#define VAD_MAX_FRAMES 400    // 单句最长 8 秒

#define WIFI_TIMEOUT_MS 20000

static i2s_chan_handle_t s_rx = NULL;
static EventGroupHandle_t s_wifi_events;
static bool s_wifi_ready = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// ==========================================================================
// 工具
// ==========================================================================
static void fmt_db(double db, char *out, size_t len)
{
    int v = (int)lround(db * 10.0);
    if (v > 0) v = 0;
    if (v < -990) v = -990;
    snprintf(out, len, "-%d.%d", (-v) / 10, (-v) % 10);
}

// ==========================================================================
// WiFi
// ==========================================================================
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        static int retries = 0;
        if (++retries <= 10) {
            printf("[wifi] 断开，重连第 %d 次\n", retries);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        printf("[wifi] 已连接，IP=" IPSTR "\n", IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_stack_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // 关省电，否则语音包会抖
}

static bool wifi_connect(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = (pass[0] == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("[wifi] 正在连 \"%s\" ...\n", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        s_wifi_ready = true;
        return true;
    }
    printf("[wifi] 连接失败（%s）\n", (bits & WIFI_FAIL_BIT) ? "密码错/连不上" : "超时");
    return false;
}

static void wifi_scan_and_print(void)
{
    printf("\n[wifi] 扫描附近 AP（挑一个和电脑同网段的填进 menuconfig）...\n");
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        printf("[wifi] 扫描失败\n");
        return;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        printf("[wifi] 没扫到任何 AP\n");
        return;
    }
    if (num > 20) num = 20;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) return;
    if (esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        for (int i = 0; i < num; ++i) {
            printf("  %2d. %-28s  %4d dBm  auth=%d%s\n", i + 1, (const char *)recs[i].ssid, recs[i].rssi,
                   recs[i].authmode, recs[i].authmode == WIFI_AUTH_OPEN ? "  (开放)" : "");
        }
    }
    free(recs);
    printf("\n");
}

// ==========================================================================
// INMP441 麦克风（标准 I2S）
// ==========================================================================
static esp_err_t mic_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = FRAME_SAMPLES;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) return err;

    // 32 bit 槽：INMP441 的 24 bit 数据左对齐在 32 bit 里，16 bit 槽会错位成低频漂移。
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = MIC_SCK_GPIO,
                .ws = MIC_WS_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din = MIC_SD_GPIO,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
    };
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // L/R 接 GND = 左声道

    err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    return i2s_channel_enable(s_rx);
}

// 读一帧并转成 int16：32 bit 槽 -> 右移 16 位 -> 乘增益 -> 限幅。
// 返回实际样本数；不足一帧就返回 0，调用方跳过这轮。
static size_t read_frame(int16_t *out)
{
    static int32_t raw[FRAME_SAMPLES];
    size_t got = 0;
    if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(200)) != ESP_OK) {
        return 0;
    }
    const size_t n = got / sizeof(int32_t);
    for (size_t i = 0; i < n; ++i) {
        int32_t v = (raw[i] >> 16) * MIC_GAIN;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
    return n;
}

static double frame_db(const int16_t *buf, size_t n, int *peak)
{
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += buf[i];
    const double mean = sum / (double)n;

    double sq = 0;
    int pk = 0;
    for (size_t i = 0; i < n; ++i) {
        const double v = (double)buf[i] - mean;
        sq += v * v;
        const int a = (int)fabs(v);
        if (a > pk) pk = a;
    }
    if (peak) *peak = pk;
    return 20.0 * log10(sqrt(sq / (double)n) / 32768.0 + 1e-9);
}

// ==========================================================================
// TCP 客户端
// ==========================================================================
static int lamp_tcp_open(const char *host, int port, int timeout_ms)
{
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        printf("[net] 服务器地址不合法：%s\n", host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&dest, sizeof(dest));
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(fd, &wf);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    r = select(fd + 1, NULL, &wf, NULL, &tv);
    if (r <= 0) {
        close(fd);
        return -1;
    }
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // 20 ms 一包，别攒
    return fd;
}

static bool send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_pcm(int fd, const int16_t *buf, size_t samples)
{
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "PCM %u\n", (unsigned)(samples * sizeof(int16_t)));
    if (!send_all(fd, hdr, (size_t)n)) return false;
    return send_all(fd, buf, samples * sizeof(int16_t));
}

// 读一行（最多 len-1 字节），超时返回 -1
static int read_line(int fd, char *buf, size_t len, int timeout_ms)
{
    size_t i = 0;
    while (i + 1 < len) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r <= 0) return -1;
        char c;
        r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') {
            buf[i] = 0;
            return (int)i;
        }
        buf[i++] = c;
    }
    buf[i] = 0;
    return (int)i;
}

// 一段话说完：收尾、等回答、逐行打印
static void finish_utterance(int fd, size_t frames)
{
    printf("[net] 说完了，共 %.2f 秒，等识别和回答...\n", frames * 0.02);
    if (!send_all(fd, "END\n", 4)) {
        printf("[net] 发送 END 失败\n");
        close(fd);
        return;
    }

    printf("\n-------------- 这一轮 --------------\n");
    char line[512];
    while (read_line(fd, line, sizeof(line), 120000) >= 0) {
        if (strncmp(line, "HEARD ", 6) == 0) {
            printf("你说 ：%s\n", line + 6);
        } else if (strncmp(line, "REPLY ", 6) == 0) {
            printf("台灯 ：%s\n", line + 6);
        } else if (strcmp(line, "DONE") == 0) {
            printf("------------------------------------\n\n");
            break;
        } else if (strncmp(line, "ERR", 3) == 0) {
            printf("[服务端错误] %s\n\n", line);
            break;
        }
    }
    close(fd);
}

// ==========================================================================
// 主流程
// ==========================================================================
void app_main(void)
{
    printf("\n=== LAMP-1 语音客户端 ===\n");
    printf("麦克风：INMP441 I2S  SCK=GPIO%d  WS=GPIO%d  SD=GPIO%d  @ %d Hz\n", MIC_SCK_GPIO, MIC_WS_GPIO,
           MIC_SD_GPIO, SAMPLE_RATE_HZ);
    printf("服务器：%s:%d\n", CONFIG_LAMP_SERVER_HOST, CONFIG_LAMP_SERVER_PORT);

    wifi_stack_init();

    const char *ssid = CONFIG_LAMP_WIFI_SSID;
    const char *pass = CONFIG_LAMP_WIFI_PASS;
    if (ssid[0] == 0) {
        printf("\n[wifi] 还没配置 SSID。\n");
        wifi_scan_and_print();
        printf("[wifi] 请把 SSID/密码填进 menuconfig（LAMP voice link），重新编译烧录。\n");
        printf("[wifi] 也顺手确认电脑 IP：ipconfig\n\n");
    } else if (!wifi_connect(ssid, pass)) {
        wifi_scan_and_print();
    }

    if (mic_start() != ESP_OK) {
        printf("!! 麦克风 I2S 初始化失败\n");
        return;
    }
    printf("麦克风已启动，开始监听...\n\n");

    static int16_t preroll[PREROLL_FRAMES][FRAME_SAMPLES];
    int preroll_idx = 0;
    int preroll_filled = 0;

    bool speaking = false;
    int fd = -1;
    int start_run = 0;
    int silence_run = 0;
    size_t utt_frames = 0;
    int peak = 0;
    uint32_t last_status = 0;

    while (true) {
        int16_t frame[FRAME_SAMPLES];
        if (read_frame(frame) < FRAME_SAMPLES) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const double db = frame_db(frame, FRAME_SAMPLES, &peak);
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        // 维护 preroll 环形缓冲
        memcpy(preroll[preroll_idx], frame, sizeof(frame));
        preroll_idx = (preroll_idx + 1) % PREROLL_FRAMES;
        if (preroll_filled < PREROLL_FRAMES) preroll_filled++;

        if (!speaking) {
            if (db > VAD_START_DB) {
                if (++start_run >= VAD_START_FRAMES) {
                    start_run = 0;
                    silence_run = 0;
                    utt_frames = 0;
                    char s[8];
                    fmt_db(db, s, sizeof(s));

                    if (!s_wifi_ready) {
                        printf("[vad] 有声音（%s dBFS）但 WiFi 没连上，跳过\n", s);
                        continue;
                    }
                    fd = lamp_tcp_open(CONFIG_LAMP_SERVER_HOST, CONFIG_LAMP_SERVER_PORT, 3000);
                    if (fd < 0) {
                        printf("[net] 连不上 %s:%d，跳过这一句\n", CONFIG_LAMP_SERVER_HOST,
                               CONFIG_LAMP_SERVER_PORT);
                        continue;
                    }

                    printf("[vad] 检测到说话（%s dBFS），开始上传...\n", s);
                    speaking = true;

                    // 先把 preroll 按时间顺序补发，再接实时帧
                    const int oldest = (preroll_idx + PREROLL_FRAMES - preroll_filled) % PREROLL_FRAMES;
                    for (int k = 0; k < preroll_filled; ++k) {
                        const int idx = (oldest + k) % PREROLL_FRAMES;
                        send_pcm(fd, preroll[idx], FRAME_SAMPLES);
                        utt_frames++;
                    }
                }
            } else {
                start_run = 0;
                if (now - last_status > 3000) {
                    last_status = now;
                    char s[8];
                    fmt_db(db, s, sizeof(s));
                    printf("[idle] 环境电平 %s dBFS，等待说话\n", s);
                }
            }
            continue;
        }

        // --- 说话中：实时上传 ---
        if (!send_pcm(fd, frame, FRAME_SAMPLES)) {
            printf("[net] 发送失败，放弃这一句\n");
            close(fd);
            speaking = false;
            preroll_filled = 0;
            continue;
        }
        utt_frames++;

        if (db < VAD_END_DB) {
            silence_run++;
        } else {
            silence_run = 0;
        }

        if (silence_run >= VAD_END_FRAMES || utt_frames >= VAD_MAX_FRAMES) {
            speaking = false;
            if (utt_frames >= VAD_MIN_FRAMES) {
                finish_utterance(fd, utt_frames);
            } else {
                printf("[vad] 太短，丢弃\n");
                close(fd);
            }
            utt_frames = 0;
            silence_run = 0;
            preroll_filled = 0;
        }
    }
}
