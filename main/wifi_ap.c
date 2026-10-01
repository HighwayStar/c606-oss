/*
 * Wi-Fi access point for the file transfer page (webfs.c).
 *
 * The radio is only brought up on demand (Settings -> System -> Wi-Fi
 * transfer): esp_wifi is initialised on start and fully torn down on stop,
 * so it costs no RAM or power otherwise. An esp_timer checks once a minute
 * and stops the AP after WIFI_AP_IDLE_MIN minutes without a station or an
 * HTTP request.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "board.h"
#include "webfs.h"
#include "wifi_ap.h"

static const char *TAG = "wifi";

#define NVS_NS  "c606oss"
#define NVS_KEY "wifipw"
#define PW_LEN  10

static SemaphoreHandle_t s_lock;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_idle_timer;
static bool s_active;
static volatile int s_clients;
static volatile int64_t s_last_us;
static char s_ssid[33];
static char s_pw[PW_LEN + 1];

/* no 0/O/1/l/I: the password is read off the screen */
static const char k_pw_chars[] = "abcdefghijkmnpqrstuvwxyz23456789";

static void load_password(void)
{
    nvs_handle_t h;
    size_t n = sizeof s_pw;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        h = 0;
    } else if (nvs_get_str(h, NVS_KEY, s_pw, &n) == ESP_OK && strlen(s_pw) >= 8) {
        nvs_close(h);
        return;
    }
    for (int i = 0; i < PW_LEN; i++) s_pw[i] = k_pw_chars[esp_random() % (sizeof k_pw_chars - 1)];
    s_pw[PW_LEN] = 0;
    if (h) {
        nvs_set_str(h, NVS_KEY, s_pw);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void make_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    int n = 0;
    for (const char *p = BOARD_NAME; *p && n < 20; p++) {
        if (*p != ' ') s_ssid[n++] = *p;   /* "CC700 Pro" -> "CC700Pro" */
    }
    snprintf(s_ssid + n, sizeof s_ssid - n, "-%02X%02X", mac[4], mac[5]);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
        ESP_LOGI(TAG, "client connected (%d)", s_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clients > 0) s_clients--;
        ESP_LOGI(TAG, "client left (%d)", s_clients);
    }
    s_last_us = esp_timer_get_time();
}

static void idle_check(void *arg)
{
    if (!s_active || s_clients > 0) return;
    if (esp_timer_get_time() - s_last_us > (int64_t)WIFI_AP_IDLE_MIN * 60 * 1000000) {
        ESP_LOGI(TAG, "idle for %d min, switching off", WIFI_AP_IDLE_MIN);
        wifi_ap_stop();
    }
}

static esp_err_t init_once(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG, "event");
    const esp_timer_create_args_t t = { .callback = idle_check, .name = "wifi_idle" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&t, &s_idle_timer), TAG, "timer");
    load_password();
    make_ssid();
    return ESP_OK;
}

static void teardown(void)
{
    webfs_stop();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_clients = 0;
}

esp_err_t wifi_ap_start(void)
{
    ESP_RETURN_ON_ERROR(init_once(), TAG, "init");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_active) goto out;

    s_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&ic);
    if (err != ESP_OK) goto fail;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* nothing to remember between starts */

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, s_ssid, sizeof wc.ap.ssid);
    wc.ap.ssid_len = strlen(s_ssid);
    strncpy((char *)wc.ap.password, s_pw, sizeof wc.ap.password);
    wc.ap.channel = 6;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;
    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) goto fail;
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &wc)) != ESP_OK) goto fail;
    if ((err = esp_wifi_start()) != ESP_OK) goto fail;
    if ((err = webfs_start()) != ESP_OK) goto fail;

    s_active = true;
    s_last_us = esp_timer_get_time();
    esp_timer_start_periodic(s_idle_timer, 60 * 1000000ULL);
    ESP_LOGI(TAG, "AP \"%s\" up, " WIFI_AP_URL, s_ssid);
    goto out;
fail:
    ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
    teardown();
out:
    xSemaphoreGive(s_lock);
    return err;
}

void wifi_ap_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {
        esp_timer_stop(s_idle_timer);
        s_active = false;
        teardown();
        ESP_LOGI(TAG, "AP off");
    }
    xSemaphoreGive(s_lock);
}

bool wifi_ap_active(void) { return s_active; }
const char *wifi_ap_ssid(void) { return s_ssid; }
const char *wifi_ap_password(void) { return s_pw; }
int wifi_ap_clients(void) { return s_clients; }
void wifi_ap_touch(void) { s_last_us = esp_timer_get_time(); }

void wifi_ap_qr_text(char *buf, int n)
{
    /* the SSID and password only use characters that need no escaping */
    snprintf(buf, n, "WIFI:T:WPA;S:%s;P:%s;;", s_ssid, s_pw);
}
