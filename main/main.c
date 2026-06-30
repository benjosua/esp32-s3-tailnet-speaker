#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "led_strip.h"
#include "microlink.h"

#define TAG "TailSpeaker"

#define DEVICE_NAME "waveshare-speaker-2fa9"
#define WIFI_CONNECTED_BIT BIT0

#define LED_GPIO GPIO_NUM_38
#define LED_COUNT 7

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define I2C_FREQ 100000
#define TCA9555_ADDR 0x20

#define I2S_MCLK GPIO_NUM_12
#define I2S_BCLK GPIO_NUM_13
#define I2S_WS   GPIO_NUM_14
#define I2S_DOUT GPIO_NUM_16

#define SAMPLE_RATE 24000
#define BLOCK_SAMPLES 256
#define AUDIO_RING_BYTES (96 * 1024)

#define FRAME_JSON  1
#define FRAME_AUDIO 2
#define FRAME_MIC   3
#define MAX_JSON_FRAME 2048
#define MAX_AUDIO_FRAME 4096

static EventGroupHandle_t wifi_event_group;
static led_strip_handle_t led_strip;
static esp_codec_dev_handle_t out_dev;
static i2c_master_bus_handle_t i2c_bus;
static RingbufHandle_t audio_ring;
static SemaphoreHandle_t controller_lock;

static microlink_t *ml;
static microlink_tcp_socket_t *controller_sock;

static volatile bool wifi_ready;
static volatile bool tailnet_ready;
static volatile bool controller_ready;
static volatile bool time_synced;
static volatile bool alarm_active;
static volatile bool remote_tone_active;
static volatile int64_t remote_tone_until_us;
static volatile float remote_tone_freq = 440.0f;
static volatile int scheduled_hh = -1;
static volatile int scheduled_mm = -1;
static volatile time_t alarm_due_epoch;
static int math_a = 5;
static int math_b = 45;
static char math_op = '+';
static int math_answer = 50;
static int current_volume = CONFIG_SPEAKER_DEFAULT_VOLUME;

static enum {
    LED_IDLE = 0,
    LED_TAILNET_CONNECTING,
    LED_SPEAKING,
    LED_LISTENING,
    LED_ERROR,
    LED_CUSTOM,
} led_mode = LED_TAILNET_CONNECTING;
static uint8_t custom_r, custom_g, custom_b;

static esp_err_t send_json_event(const char *event, const char *extra_json);

static void audio_ring_clear(void) {
    if (!audio_ring) return;
    size_t item_len = 0;
    uint8_t *item = NULL;
    while ((item = (uint8_t *)xRingbufferReceive(audio_ring, &item_len, 0)) != NULL) {
        vRingbufferReturnItem(audio_ring, item);
    }
}

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    i2c_master_dev_handle_t dev;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_FREQ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev), TAG, "add i2c dev");
    uint8_t data[2] = {reg, val};
    esp_err_t ret = i2c_master_transmit(dev, data, sizeof(data), 1000);
    i2c_master_bus_rm_device(dev);
    return ret;
}

static void init_i2c_and_amp(void) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus));

    // TCA9555 IO8 = port1 bit0. Set speaker amplifier enable high.
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_write_reg(TCA9555_ADDR, 0x03, 0x01));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_write_reg(TCA9555_ADDR, 0x07, 0xFE));
}

static void init_leds(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {.resolution_hz = 10 * 1000 * 1000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

static void set_leds(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_strip) return;
    for (int i = 0; i < LED_COUNT; ++i) led_strip_set_pixel(led_strip, i, r, g, b);
    led_strip_refresh(led_strip);
}

static void led_task(void *arg) {
    bool pulse = false;
    while (1) {
        pulse = !pulse;
        if (alarm_active) {
            set_leds(pulse ? 80 : 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(180));
            continue;
        }

        switch (led_mode) {
            case LED_TAILNET_CONNECTING:
                set_leds(0, 0, pulse ? 48 : 8);
                vTaskDelay(pdMS_TO_TICKS(350));
                break;
            case LED_SPEAKING:
                set_leds(0, 0, 80);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case LED_LISTENING:
                set_leds(0, 80, 80);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case LED_ERROR:
                set_leds(pulse ? 80 : 16, pulse ? 40 : 8, 0);
                vTaskDelay(pdMS_TO_TICKS(400));
                break;
            case LED_CUSTOM:
                set_leds(custom_r, custom_g, custom_b);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case LED_IDLE:
            default:
                set_leds(0, 48, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
        }
    }
}

static void init_audio(void) {
    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = NULL, .tx_handle = tx_handle};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    es8311_codec_cfg_t es8311_cfg = {0};
    es8311_cfg.ctrl_if = ctrl_if;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = GPIO_NUM_NC;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    assert(codec_if);

    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if};
    out_dev = esp_codec_dev_new(&dev_cfg);
    assert(out_dev);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(out_dev, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(out_dev, current_volume));

    audio_ring = xRingbufferCreate(AUDIO_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!audio_ring) ESP_LOGE(TAG, "Failed to create audio ringbuffer");
}

static void make_mellow_pulse(int16_t *buf, int samples, float *phase, int *t, float freq, float amp) {
    for (int i = 0; i < samples; ++i) {
        int tt = (*t)++;
        int beat = tt % SAMPLE_RATE;
        float env = (beat < SAMPLE_RATE / 3) ? amp : 0.0f;
        *phase += 2.0f * (float)M_PI * freq / SAMPLE_RATE;
        if (*phase > 2.0f * (float)M_PI) *phase -= 2.0f * (float)M_PI;
        buf[i] = (int16_t)(sinf(*phase) * env);
    }
}

static void audio_task(void *arg) {
    int16_t silence[BLOCK_SAMPLES] = {0};
    int16_t tone_buf[BLOCK_SAMPLES];
    float phase = 0.0f;
    int t = 0;
    bool was_streaming = false;

    while (1) {
        if (!out_dev) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (alarm_active) {
            make_mellow_pulse(tone_buf, BLOCK_SAMPLES, &phase, &t, 440.0f, 6800.0f);
            esp_codec_dev_write(out_dev, tone_buf, sizeof(tone_buf));
            continue;
        }

        if (remote_tone_active) {
            if (now >= remote_tone_until_us) {
                remote_tone_active = false;
                led_mode = LED_IDLE;
                send_json_event("audio_stopped", "{\"source\":\"tone\"}");
            } else {
                make_mellow_pulse(tone_buf, BLOCK_SAMPLES, &phase, &t, remote_tone_freq, 6200.0f);
                esp_codec_dev_write(out_dev, tone_buf, sizeof(tone_buf));
                continue;
            }
        }

        size_t item_len = 0;
        uint8_t *item = audio_ring ? (uint8_t *)xRingbufferReceiveUpTo(audio_ring, &item_len, pdMS_TO_TICKS(5), 4096) : NULL;
        if (item && item_len > 0) {
            if (!was_streaming) {
                was_streaming = true;
                led_mode = LED_SPEAKING;
                send_json_event("audio_started", "{\"source\":\"stream\"}");
            }
            esp_codec_dev_write(out_dev, item, item_len);
            vRingbufferReturnItem(audio_ring, item);
            continue;
        }

        if (was_streaming) {
            was_streaming = false;
            led_mode = LED_IDLE;
            send_json_event("audio_stopped", "{\"source\":\"stream\"}");
        }
        esp_codec_dev_write(out_dev, silence, sizeof(silence));
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void make_math_question(void) {
    uint32_t r = esp_random();
    math_op = (r & 1) ? '+' : 'x';
    if (math_op == '+') {
        math_a = 5 + (r % 45);
        math_b = 5 + ((r >> 8) % 50);
        math_answer = math_a + math_b;
    } else {
        math_a = 2 + (r % 10);
        math_b = 2 + ((r >> 8) % 10);
        math_answer = math_a * math_b;
    }
}

static bool parse_alarm_time(const char *s, int *hh, int *mm) {
    if (!s || !isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) return false;
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    s += 2;
    if (*s != ':') return false;
    s++;
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) return false;
    int m = (s[0] - '0') * 10 + (s[1] - '0');
    if (h > 23 || m > 59) return false;
    *hh = h;
    *mm = m;
    return true;
}

static void schedule_alarm_local(int hh, int mm) {
    time_t now = time(NULL);
    struct tm local_now;
    localtime_r(&now, &local_now);
    local_now.tm_hour = hh;
    local_now.tm_min = mm;
    local_now.tm_sec = 0;
    time_t due = mktime(&local_now);
    if (due <= now) due += 24 * 60 * 60;
    alarm_due_epoch = due;
    scheduled_hh = hh;
    scheduled_mm = mm;
    ESP_LOGI(TAG, "Scheduled alarm %02d:%02d", hh, mm);
}

static void schedule_task(void *arg) {
    while (1) {
        if (!alarm_active && alarm_due_epoch > 0 && time_synced && time(NULL) >= alarm_due_epoch) {
            make_math_question();
            alarm_active = true;
            alarm_due_epoch = 0;
            led_mode = LED_IDLE;
            send_json_event("alarm_active", NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static esp_err_t send_frame(uint8_t type, const void *data, uint32_t len) {
    if (!controller_ready || !controller_sock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(controller_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!controller_sock || !microlink_tcp_is_connected(controller_sock)) {
        xSemaphoreGive(controller_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t header[5] = {
        type,
        (uint8_t)(len >> 24),
        (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),
        (uint8_t)len,
    };
    esp_err_t err = microlink_tcp_send(controller_sock, header, sizeof(header));
    if (err == ESP_OK && len > 0) err = microlink_tcp_send(controller_sock, data, len);
    xSemaphoreGive(controller_lock);
    return err;
}

static esp_err_t send_json_event(const char *event, const char *extra_json) {
    char ip[16] = "0.0.0.0";
    if (ml && microlink_get_vpn_ip(ml)) microlink_ip_to_str(microlink_get_vpn_ip(ml), ip);
    char json[768];
    int n = snprintf(json, sizeof(json),
        "{\"type\":\"event\",\"event\":\"%s\",\"device\":\"%s\","
        "\"vpn_ip\":\"%s\",\"wifi\":%s,\"tailnet\":%s,\"controller\":%s,"
        "\"alarm_active\":%s,\"scheduled\":\"%02d:%02d\",\"volume\":%d%s%s}",
        event, DEVICE_NAME, ip,
        wifi_ready ? "true" : "false", tailnet_ready ? "true" : "false", controller_ready ? "true" : "false",
        alarm_active ? "true" : "false",
        scheduled_hh < 0 ? 0 : scheduled_hh, scheduled_mm < 0 ? 0 : scheduled_mm,
        current_volume,
        extra_json ? ",\"extra\":" : "", extra_json ? extra_json : "");
    if (n <= 0 || n >= (int)sizeof(json)) return ESP_ERR_NO_MEM;
    return send_frame(FRAME_JSON, json, (uint32_t)n);
}

static void handle_json_command(const char *json, size_t len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        send_json_event("error", "{\"message\":\"bad_json\"}");
        return;
    }
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) cmd = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        send_json_event("error", "{\"message\":\"missing_cmd\"}");
        return;
    }

    if (strcmp(cmd->valuestring, "status_request") == 0) {
        send_json_event("status", NULL);
    } else if (strcmp(cmd->valuestring, "set_led") == 0) {
        custom_r = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "r"));
        custom_g = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "g"));
        custom_b = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "b"));
        led_mode = LED_CUSTOM;
        send_json_event("led_set", NULL);
    } else if (strcmp(cmd->valuestring, "set_volume") == 0) {
        int v = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "volume"));
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        current_volume = v;
        if (out_dev) esp_codec_dev_set_out_vol(out_dev, current_volume);
        send_json_event("volume_set", NULL);
    } else if (strcmp(cmd->valuestring, "play_tone") == 0) {
        double seconds = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "seconds"));
        double freq = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "frequency"));
        if (seconds <= 0.0) seconds = 3.0;
        if (freq < 100.0) freq = 440.0;
        remote_tone_freq = (float)freq;
        remote_tone_until_us = esp_timer_get_time() + (int64_t)(seconds * 1000000.0);
        remote_tone_active = true;
        led_mode = LED_SPEAKING;
        send_json_event("audio_started", "{\"source\":\"tone\"}");
    } else if (strcmp(cmd->valuestring, "audio_stop") == 0) {
        audio_ring_clear();
        remote_tone_active = false;
        led_mode = LED_IDLE;
        send_json_event("audio_stopped", "{\"source\":\"remote_stop\"}");
    } else if (strcmp(cmd->valuestring, "alarm_start") == 0) {
        make_math_question();
        alarm_active = true;
        send_json_event("alarm_active", NULL);
    } else if (strcmp(cmd->valuestring, "alarm_stop") == 0) {
        alarm_active = false;
        led_mode = LED_IDLE;
        send_json_event("alarm_stopped", "{\"source\":\"remote\"}");
    } else if (strcmp(cmd->valuestring, "schedule_set") == 0) {
        int hh = -1, mm = -1;
        const cJSON *time_item = cJSON_GetObjectItem(root, "time");
        if (cJSON_IsString(time_item)) parse_alarm_time(time_item->valuestring, &hh, &mm);
        const cJSON *hh_item = cJSON_GetObjectItem(root, "hh");
        const cJSON *mm_item = cJSON_GetObjectItem(root, "mm");
        if (cJSON_IsNumber(hh_item) && cJSON_IsNumber(mm_item)) {
            hh = hh_item->valueint;
            mm = mm_item->valueint;
        }
        if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59 && time_synced) {
            schedule_alarm_local(hh, mm);
            send_json_event("schedule_set", NULL);
        } else {
            send_json_event("error", "{\"message\":\"bad_schedule_or_clock_unsynced\"}");
        }
    } else if (strcmp(cmd->valuestring, "speak") == 0) {
        // Firmware has no local TTS. Server should stream PCM via FRAME_AUDIO.
        send_json_event("error", "{\"message\":\"speak_requires_streamed_audio_pcm\"}");
    } else if (strcmp(cmd->valuestring, "play_audio") == 0) {
        audio_ring_clear();
        led_mode = LED_SPEAKING;
        send_json_event("audio_ready", "{\"format\":\"pcm_s16le_24k_mono\"}");
    } else {
        send_json_event("error", "{\"message\":\"unknown_cmd\"}");
    }
    cJSON_Delete(root);
}

static bool recv_exact(microlink_tcp_socket_t *sock, void *buf, size_t len, uint32_t timeout_ms) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        int n = microlink_tcp_recv(sock, p + got, len - got, timeout_ms);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static void controller_session(microlink_tcp_socket_t *sock) {
    controller_sock = sock;
    controller_ready = true;
    led_mode = LED_IDLE;
    send_json_event("hello", "{\"protocol\":1,\"audio\":\"pcm_s16le_24k_mono\"}");

    uint8_t *payload = malloc(MAX_JSON_FRAME > MAX_AUDIO_FRAME ? MAX_JSON_FRAME : MAX_AUDIO_FRAME);
    if (!payload) {
        send_json_event("error", "{\"message\":\"alloc_failed\"}");
        return;
    }

    while (microlink_tcp_is_connected(sock)) {
        uint8_t header[5];
        if (!recv_exact(sock, header, sizeof(header), 30000)) break;
        uint8_t type = header[0];
        uint32_t len = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 8) | header[4];
        if ((type == FRAME_JSON && len > MAX_JSON_FRAME) || (type == FRAME_AUDIO && len > MAX_AUDIO_FRAME) || len == 0) {
            ESP_LOGW(TAG, "Bad frame type=%u len=%lu", type, (unsigned long)len);
            break;
        }
        if (!recv_exact(sock, payload, len, 10000)) break;
        if (type == FRAME_JSON) {
            handle_json_command((const char *)payload, len);
        } else if (type == FRAME_AUDIO) {
            if (audio_ring && xRingbufferSend(audio_ring, payload, len, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Audio ring full; dropping %lu bytes", (unsigned long)len);
            }
        } else {
            ESP_LOGW(TAG, "Ignoring unsupported frame type=%u", type);
        }
    }

    free(payload);
}

static uint32_t resolve_server_ip(void) {
    const char *host = CONFIG_SPEAKER_SERVER_HOST;
    if (!host || host[0] == '\0') return 0;
    uint32_t ip = microlink_parse_ip(host);
    if (ip != 0) return ip;
    if (ml) return microlink_resolve(ml, host);
    return 0;
}

static void controller_task(void *arg) {
    while (1) {
        if (!ml || !microlink_is_connected(ml)) {
            controller_ready = false;
            controller_sock = NULL;
            led_mode = LED_TAILNET_CONNECTING;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t server_ip = resolve_server_ip();
        if (server_ip == 0) {
            ESP_LOGW(TAG, "No/invalid CONFIG_SPEAKER_SERVER_HOST; remote control disabled");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        char ip_str[16];
        microlink_ip_to_str(server_ip, ip_str);
        ESP_LOGI(TAG, "Connecting controller %s:%d", ip_str, CONFIG_SPEAKER_SERVER_PORT);
        microlink_tcp_socket_t *sock = microlink_tcp_connect(ml, server_ip, CONFIG_SPEAKER_SERVER_PORT, 30000);
        if (!sock) {
            ESP_LOGW(TAG, "Controller connect failed");
            led_mode = LED_ERROR;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "Controller connected");
        controller_session(sock);

        controller_ready = false;
        controller_sock = NULL;
        microlink_tcp_close(sock);
        ESP_LOGW(TAG, "Controller disconnected; reconnecting");
        led_mode = tailnet_ready ? LED_IDLE : LED_TAILNET_CONNECTING;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void on_time_sync(struct timeval *tv) {
    time_synced = true;
    ESP_LOGI(TAG, "Internet clock synced");
}

static void start_sntp(void) {
    static bool started = false;
    if (started) return;
    started = true;
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_ready = false;
        ESP_LOGW(TAG, "WiFi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_ready = true;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_sntp();
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {.sta = {.threshold.authmode = WIFI_AUTH_WPA2_PSK}};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_ML_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_ML_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi connecting to '%s'", CONFIG_ML_WIFI_SSID);
}

static void on_ml_state(microlink_t *ml_handle, microlink_state_t state, void *user_data) {
    const char *names[] = {"IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING", "CONNECTED", "RECONNECTING", "ERROR"};
    const char *name = (state >= 0 && state < (int)(sizeof(names) / sizeof(names[0]))) ? names[state] : "UNKNOWN";
    ESP_LOGI(TAG, "MicroLink state: %s", name);
    tailnet_ready = (state == ML_STATE_CONNECTED);
    if (tailnet_ready) {
        char ip[16];
        microlink_ip_to_str(microlink_get_vpn_ip(ml_handle), ip);
        ESP_LOGI(TAG, "Tailnet connected: %s", ip);
        led_mode = controller_ready ? LED_IDLE : LED_TAILNET_CONNECTING;
    } else if (state == ML_STATE_ERROR) {
        led_mode = LED_ERROR;
    } else if (!controller_ready) {
        led_mode = LED_TAILNET_CONNECTING;
    }
}

static void start_microlink(void) {
    if (CONFIG_ML_TAILSCALE_AUTH_KEY[0] == '\0') {
        ESP_LOGW(TAG, "CONFIG_ML_TAILSCALE_AUTH_KEY empty; skipping MicroLink");
        led_mode = LED_ERROR;
        return;
    }
    uint32_t priority = 0;
#ifdef CONFIG_ML_PRIORITY_PEER_IP
    if (CONFIG_ML_PRIORITY_PEER_IP[0] != '\0') priority = microlink_parse_ip(CONFIG_ML_PRIORITY_PEER_IP);
#endif
    microlink_config_t config = {
        .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = DEVICE_NAME,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 13,
        .priority_peer_ip = priority,
    };
    ml = microlink_init(&config);
    if (!ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        led_mode = LED_ERROR;
        return;
    }
    microlink_set_state_callback(ml, on_ml_state, NULL);
    ESP_ERROR_CHECK_WITHOUT_ABORT(microlink_start(ml));
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    char clock_text[32] = "unsynced";
    if (time_synced) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        snprintf(clock_text, sizeof(clock_text), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    }
    char vpn_ip[16] = "none";
    if (ml && microlink_get_vpn_ip(ml)) microlink_ip_to_str(microlink_get_vpn_ip(ml), vpn_ip);

    char html[1800];
    snprintf(html, sizeof(html),
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Tailnet Speaker</title><style>body{font-family:system-ui;background:#111;color:#eee;margin:2rem}"
        "main{max-width:44rem;margin:auto}.card{background:#222;border-radius:16px;padding:1.5rem}"
        "input,button{font-size:1.1rem;padding:.8rem;border-radius:10px;border:0;width:100%%;box-sizing:border-box;margin:.4rem 0}"
        "button{background:#2563eb;color:white}.math{font-size:2rem;font-weight:800}.quiet{color:#aaa}</style></head>"
        "<body><main><div class='card'><h1>Tailnet Speaker</h1>"
        "<p>WiFi: <b>%s</b> Tailnet: <b>%s</b> Controller: <b>%s</b></p>"
        "<p>VPN IP: <b>%s</b> Clock: <b>%s</b> Volume: <b>%d</b></p>",
        wifi_ready ? "connected" : "down", tailnet_ready ? "connected" : "down", controller_ready ? "connected" : "down",
        vpn_ip, clock_text, current_volume);
    httpd_resp_sendstr_chunk(req, html);

    if (alarm_active) {
        snprintf(html, sizeof(html),
            "<h2>Alarm active</h2><div class='math'>%d %c %d = ?</div>"
            "<form method='POST' action='/answer'><input name='answer' inputmode='numeric' autofocus><button>Stop alarm</button></form>",
            math_a, math_op, math_b);
        httpd_resp_sendstr_chunk(req, html);
    } else {
        snprintf(html, sizeof(html),
            "<h2>Set local 24h alarm</h2><form method='POST' action='/schedule'>"
            "<input name='alarm_time' type='time' required><button>Save alarm</button></form>"
            "<p class='quiet'>Scheduled: %s%02d:%02d%s</p>",
            scheduled_hh < 0 ? "none" : "", scheduled_hh < 0 ? 0 : scheduled_hh, scheduled_mm < 0 ? 0 : scheduled_mm,
            scheduled_hh < 0 ? "" : "");
        httpd_resp_sendstr_chunk(req, html);
    }
    httpd_resp_sendstr_chunk(req, "<p class='quiet'>Remote control uses MicroLink/Tailscale TCP frames.</p></div></main></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t schedule_post_handler(httpd_req_t *req) {
    char body[96] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return ESP_FAIL;
    char *p = strstr(body, "alarm_time=");
    int hh = -1, mm = -1;
    if (p) {
        p += strlen("alarm_time=");
        if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1])) {
            hh = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
            if (p[0] == '%' && p[1] == '3' && (p[2] == 'A' || p[2] == 'a')) p += 3;
            else if (*p == ':') p++;
            if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1])) mm = (p[0] - '0') * 10 + (p[1] - '0');
        }
    }
    if (!time_synced || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Clock not synced or bad time");
        return ESP_OK;
    }
    schedule_alarm_local(hh, mm);
    send_json_event("schedule_set", NULL);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t answer_post_handler(httpd_req_t *req) {
    char body[96] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return ESP_FAIL;
    char *p = strstr(body, "answer=");
    int answer = p ? atoi(p + strlen("answer=")) : 0;
    if (alarm_active && answer == math_answer) {
        alarm_active = false;
        led_mode = LED_IDLE;
        send_json_event("alarm_stopped", "{\"source\":\"math\"}");
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_sendstr(req, "Wrong answer");
    }
    return ESP_OK;
}

static void start_httpd(void) {
#if CONFIG_SPEAKER_ENABLE_LAN_STATUS_HTTPD
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 12288;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_uri_t schedule = {.uri = "/schedule", .method = HTTP_POST, .handler = schedule_post_handler};
        httpd_uri_t answer = {.uri = "/answer", .method = HTTP_POST, .handler = answer_post_handler};
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &schedule);
        httpd_register_uri_handler(server, &answer);
        ESP_LOGI(TAG, "LAN HTTP status server started");
    }
#endif
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32-S3 Tailnet Speaker boot");
    ESP_LOGI(TAG, "Heap: %lu PSRAM: %lu", (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    controller_lock = xSemaphoreCreateMutex();
    init_i2c_and_amp();
    init_leds();
    init_audio();

    xTaskCreate(led_task, "led", 3072, NULL, 4, NULL);
    xTaskCreate(audio_task, "audio", 6144, NULL, 6, NULL);
    xTaskCreate(schedule_task, "schedule", 4096, NULL, 4, NULL);

    wifi_init();
    start_httpd();

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    start_microlink();
    xTaskCreate(controller_task, "controller", 8192, NULL, 5, NULL);

    send_json_event("boot", NULL);
}
