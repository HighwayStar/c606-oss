/* One-shot network time assistance. Never erase NVS; settings use our own
 * namespace, and all network work runs outside the LVGL task. */
#include "wifi_aid.h"
#include "gps.h"
#include "utc.h"
#include "l10n.h"
#include "agnss.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "wifi_aid";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static char s_status[160] = "2.4 GHz Wi-Fi. Sync UTC or update A-GPS (GPS EPO).";
typedef struct { char ssid[33], password[65]; EventGroupHandle_t events; bool epo; } job_t;
#define GOT_IP BIT0
#define LOST_LINK BIT1

static void status(const char *text)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_status, sizeof s_status, "%s", text);
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%s", text);
}

void wifi_aid_status(char *out, size_t size)
{
    char snapshot[sizeof s_status];
    taskENTER_CRITICAL(&s_mux);
    memcpy(snapshot, s_status, sizeof snapshot);
    taskEXIT_CRITICAL(&s_mux);
    const char *prefix = "Wi-Fi/time error: ";
    if (!strncmp(snapshot, prefix, strlen(prefix)))
        l10n_format(out, size, "Wi-Fi/time error: %s", snapshot + strlen(prefix));
    else l10n_format(out, size, "%s", tr(snapshot));
}

void wifi_aid_load(char ssid[33], char password[65])
{
    ssid[0] = password[0] = 0;
    nvs_handle_t h;
    if (nvs_open("c606wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = 33;
    if (nvs_get_str(h, "ssid", ssid, &n) != ESP_OK) ssid[0] = 0;
    n = 65;
    if (nvs_get_str(h, "password", password, &n) != ESP_OK) password[0] = 0;
    nvs_close(h);
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    job_t *j = arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(j->events, GOT_IP);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupSetBits(j->events, LOST_LINK);
}

static void worker(void *arg)
{
    job_t *j = arg;
    bool own_loop = false, initialized = false, started = false, sntp = false;
    esp_netif_t *netif = NULL;
    esp_event_handler_instance_t ip_handler = NULL, disconnect_handler = NULL;
    esp_err_t err;
    status("Connecting to Wi-Fi...");
#define TRY(expr) do { err = (expr); if (err != ESP_OK) goto fail; } while (0)
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = esp_event_loop_create_default();
    if (err == ESP_OK) own_loop = true;
    else if (err != ESP_ERR_INVALID_STATE) goto fail;
    netif = esp_netif_create_default_wifi_sta();
    if (!netif) { err = ESP_ERR_NO_MEM; goto fail; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&init));
    initialized = true;
    /* Wi-Fi driver must not modify the vendor's saved Wi-Fi settings. */
    TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, j->ssid, strlen(j->ssid));
    memcpy(cfg.sta.password, j->password, strlen(j->password));
    cfg.sta.pmf_cfg.capable = true;
    TRY(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    TRY(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, j, &ip_handler));
    TRY(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, network_event, j, &disconnect_handler));
    TRY(esp_wifi_start());
    started = true;
    TRY(esp_wifi_connect());
    EventBits_t bits = xEventGroupWaitBits(j->events, GOT_IP | LOST_LINK, pdFALSE, pdFALSE, pdMS_TO_TICKS(25000));
    if (!(bits & GOT_IP) || (bits & LOST_LINK)) {
        status("Wi-Fi failed. Check 2.4 GHz SSID/password and signal.");
        goto done;
    }
    nvs_handle_t h;
    if (nvs_open("c606wifi", NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t save = nvs_set_str(h, "ssid", j->ssid);
        if (save == ESP_OK) save = nvs_set_str(h, "password", j->password);
        if (save == ESP_OK) save = nvs_commit(h);
        if (save != ESP_OK) ESP_LOGW(TAG, "Could not save network: %s", esp_err_to_name(save));
        nvs_close(h);
    }
    status("Wi-Fi connected. Getting UTC from NTP...");
    esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    TRY(esp_netif_sntp_init(&ntp));
    sntp = true;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000));
    if (err != ESP_OK) {
        status("NTP timeout. Check Internet / UDP 123. GPS time not sent.");
        goto done;
    }
    time_t now = time(NULL);
    if (now < 1704067200 || now > UINT32_MAX) {
        status("Invalid network time. GPS time not sent.");
        goto done;
    }
    utc_set_network((uint32_t)now);
    status("UTC synchronized. Waiting for GPS acknowledgement...");
    err = gps_assist_utc((uint32_t)time(NULL));
    if (err == ESP_OK && j->epo) {
        unsigned accepted;
        uint32_t expires;
        status("Downloading A-GPS and sending GPS EPO...");
        err = agnss_update(&accepted, &expires);
        if (err == ESP_OK) status("GPS accepted 32 EPO records. Wi-Fi off. Wait outdoors for a fix.");
        else status("A-GPS failed. Time was synced. See debug log for details.");
    } else if (err == ESP_OK) status("GPS accepted UTC. Wi-Fi off. Wait outdoors for a fix.");
    else status("UTC synced, GPS did not confirm time assistance. Wi-Fi off.");
    goto done;
fail: {
    char message[120];
    snprintf(message, sizeof message, "Wi-Fi/time error: %s", esp_err_to_name(err));
    status(message);
}
done:
    if (sntp) esp_netif_sntp_deinit();
    if (started) esp_wifi_stop();
    if (ip_handler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    if (disconnect_handler) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, disconnect_handler);
    if (initialized) esp_wifi_deinit();
    if (netif) esp_netif_destroy_default_wifi(netif);
    if (own_loop) esp_event_loop_delete_default();
    vEventGroupDelete(j->events);
    memset(j, 0, sizeof *j);
    free(j);
    taskENTER_CRITICAL(&s_mux);
    s_busy = false;
    taskEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
#undef TRY
}

static esp_err_t start(const char *ssid, const char *password, bool epo)
{
    size_t sn = strlen(ssid), pn = strlen(password);
    if (!sn || sn > 32 || pn > 64 || (pn && pn < 8)) return ESP_ERR_INVALID_ARG;
    if (pn == 64 && strspn(password, "0123456789abcdefABCDEF") != 64) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_mux);
    bool busy = s_busy;
    if (!busy) s_busy = true;
    taskEXIT_CRITICAL(&s_mux);
    if (busy) return ESP_ERR_INVALID_STATE;
    job_t *j = calloc(1, sizeof *j);
    if (j) {
        strcpy(j->ssid, ssid);
        strcpy(j->password, password);
        j->epo = epo;
        j->events = xEventGroupCreate();
        if (j->events && xTaskCreate(worker, "wifi_aid", 6144, j, 5, NULL) == pdPASS) return ESP_OK;
        if (j->events) vEventGroupDelete(j->events);
        memset(j, 0, sizeof *j);
        free(j);
    }
    taskENTER_CRITICAL(&s_mux);
    s_busy = false;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_ERR_NO_MEM;
}

esp_err_t wifi_aid_start(const char *ssid, const char *password) { return start(ssid, password, false); }
esp_err_t wifi_aid_start_epo(const char *ssid, const char *password) { return start(ssid, password, true); }
