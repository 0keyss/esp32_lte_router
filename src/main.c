#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "nvs_flash.h"
#include "esp_err.h"

#include "esp_modem_api.h"
#include "esp_modem_config.h"
#include "esp_modem_dce_config.h"
#include "esp_timer.h"

#include "esp_http_client.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"

#include "esp_wifi.h"
#include "lwip/lwip_napt.h"
#include "esp_mac.h"

#include "esp_crt_bundle.h"

#include "esp_task_wdt.h"

#define HTTP_POST_URL   "http://httpbin.org/post"   // do testów; potem Twój endpoint
#define HTTP_POST_CT    "application/json"

#define MODEM_UART_TX       27
#define MODEM_UART_RX       26
#define MODEM_UART_RTS      -1
#define MODEM_UART_CTS      -1
#define MODEM_UART_BAUD     115200

#define MODEM_PWRKEY        4
#define MODEM_FLIGHT        25
#define LED_PIN             12

#define MODEM_APN           "internet"

#define WIFI_SSID       "ESP32-LTE-Hotspot"
#define WIFI_PASS       "12345678"           // min 8 znaków
#define WIFI_CHANNEL    1
#define MAX_STA_CONN    4

static const char *TAG = "lte_pppos";

static EventGroupHandle_t event_group;
static const int CONNECT_BIT = BIT0;
static const int STOP_BIT    = BIT1;

static const int RECONNECT_BIT = BIT2;   // do event_group, sygnał „zrestartuj PPP"
static int g_reconnect_count = 0;

static esp_netif_t *g_ap_netif = NULL;

static esp_modem_dce_t *g_dce = NULL;   // udostępniamy DCE drugiemu taskowi

static void on_wifi_event(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Client connected: " MACSTR " AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Client disconnected: " MACSTR " AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

static void wifi_softap_init(void)
{
    g_ap_netif = esp_netif_create_default_wifi_ap();
    assert(g_ap_netif);

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &on_wifi_event, NULL));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    if (strlen(WIFI_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // === DNS przez DHCP - DODANE ===
ESP_ERROR_CHECK(esp_netif_dhcps_stop(g_ap_netif));

// Ustaw DNS który DHCPS będzie rozdawał klientom
esp_netif_dns_info_t dns_info = { 0 };
dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
dns_info.ip.type = ESP_IPADDR_TYPE_V4;
ESP_ERROR_CHECK(esp_netif_set_dns_info(g_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info));

// Włącz "offer DNS" w DHCPS
uint8_t dhcps_offer_dns = 1;  // OFFER_DNS flag
ESP_ERROR_CHECK(esp_netif_dhcps_option(
    g_ap_netif,
    ESP_NETIF_OP_SET,
    ESP_NETIF_DOMAIN_NAME_SERVER,
    &dhcps_offer_dns,
    sizeof(dhcps_offer_dns)
));

// Restart DHCPS z nowymi ustawieniami
ESP_ERROR_CHECK(esp_netif_dhcps_start(g_ap_netif));
    // === KONIEC DNS FIX ===

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=%s pass=%s channel=%d",
             WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
}
static void enable_napt_on_ap(void)
{
    // Włącz NAT na interfejsie AP (lwIP NAPT)
    esp_netif_t *ap = g_ap_netif;
    if (!ap) {
        ESP_LOGE(TAG, "AP netif not ready for NAPT");
        return;
    }
    
    // Pobierz uchwyt lwIP netif z esp_netif
    esp_netif_dhcps_stop(ap);  // przeładujemy DHCP po zmianach IP
    
    // ip_napt_enable_netif jest funkcją lwIP - włączamy NAT na wybranym netif
    // Korzystamy z ip_napt_enable na adresie IP AP (192.168.4.1)
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap, &ip_info);
    
    ip_napt_enable(ip_info.ip.addr, 1);
    ESP_LOGI(TAG, "NAPT enabled on AP (IP=" IPSTR ")", IP2STR(&ip_info.ip));
    
    esp_netif_dhcps_start(ap);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP connected");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "HDR %s: %s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            ESP_LOGI(TAG, "DATA (%d): %.*s", evt->data_len, evt->data_len, (char *)evt->data);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP finished");
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP error");
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void modem_power_on(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN) | (1ULL << MODEM_PWRKEY) | (1ULL << MODEM_FLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(LED_PIN, 1);

    gpio_set_level(MODEM_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(MODEM_PWRKEY, 0);

    gpio_set_level(MODEM_FLIGHT, 1);

    ESP_LOGI(TAG, "Modem power sequence done");
}

static void http_post_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int counter = 0;
    char payload[128];

    while (1) {
        // === Czekaj aż PPP będzie up ===
        while (!(xEventGroupGetBits(event_group) & CONNECT_BIT)) {
            ESP_LOGI(TAG, "http_post: waiting for PPP...");
            for (int i = 0; i < 5; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_task_wdt_reset();
            }
        }

        // === Twórz świeżego klienta dla każdej sesji PPP ===
        esp_http_client_config_t cfg = {
            .url = "https://httpbin.org/post",
            .method = HTTP_METHOD_POST,
            .event_handler = http_event_handler,
            .timeout_ms = 20000,
            .keep_alive_enable = true,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "http_client_init failed");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");

        int consecutive_failures = 0;
        const int MAX_CONSECUTIVE_FAILURES = 3;

        // === Pętla wewnętrzna: rób POST-y dopóki PPP żyje ===
        while (xEventGroupGetBits(event_group) & CONNECT_BIT) {
            esp_task_wdt_reset();

            snprintf(payload, sizeof(payload),
                     "{\"device\":\"esp32-sim7600\",\"value\":%d,\"reconnects\":%d}",
                     counter++, g_reconnect_count);

            esp_http_client_set_post_field(client, payload, strlen(payload));

            int64_t t_start = esp_timer_get_time();
            esp_err_t err = esp_http_client_perform(client);
            int64_t t_ms = (esp_timer_get_time() - t_start) / 1000;

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "POST OK, status=%d len=%" PRId64 ", took %lld ms",
                         esp_http_client_get_status_code(client),
                         esp_http_client_get_content_length(client),
                         t_ms);
                consecutive_failures = 0;
            } else {
                consecutive_failures++;
                ESP_LOGE(TAG, "POST failed (%d/%d): %s",
                         consecutive_failures, MAX_CONSECUTIVE_FAILURES,
                         esp_err_to_name(err));

                if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                    ESP_LOGW(TAG, "Too many failures, forcing PPP reconnect");
                    xEventGroupClearBits(event_group, CONNECT_BIT);
                    xEventGroupSetBits(event_group, RECONNECT_BIT);
                    break;  // wyjdź z wewnętrznej pętli, klient zostanie zniszczony
                }
            }

            esp_task_wdt_reset();

            // sleep 10 s z karmieniem WDT
            for (int i = 0; i < 10; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_task_wdt_reset();
            }
        }

        // === PPP padło lub wymusiliśmy reconnect - sprzątamy klienta ===
        ESP_LOGI(TAG, "Cleaning up HTTP client (PPP down or forced reconnect)");
        esp_http_client_cleanup(client);
    }

    // tu nigdy nie dochodzimy
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}
static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(TAG, "PPP GOT IP");
        ESP_LOGI(TAG, "IP      : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "NETMASK : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "GW      : " IPSTR, IP2STR(&event->ip_info.gw));

        esp_netif_set_default_netif(netif);

        esp_netif_dns_info_t dns_main = {0};
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main);
        ESP_LOGI(TAG, "DNS main: " IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));

        static bool napt_enabled = false;
        if (!napt_enabled) {
            enable_napt_on_ap();
            napt_enabled = true;
        }

        if (dns_main.ip.u_addr.ip4.addr == 0) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
            ESP_LOGW(TAG, "Operator DNS empty, forcing 8.8.8.8 / 1.1.1.1");
        }

        xEventGroupSetBits(event_group, CONNECT_BIT);
        xEventGroupClearBits(event_group, RECONNECT_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP LOST IP - triggering reconnect");
        xEventGroupClearBits(event_group, CONNECT_BIT);
        xEventGroupSetBits(event_group, RECONNECT_BIT);
    }
}
static void on_ppp_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP event id=%" PRId32, event_id);

    if (event_id == NETIF_PPP_ERRORUSER) {
        ESP_LOGI(TAG, "User interrupted PPP");
        xEventGroupSetBits(event_group, STOP_BIT);
    }
}

static void log_signal_quality(esp_modem_dce_t *dce, const char *context)
{
    char resp[128] = {0};
    esp_err_t err = esp_modem_at(dce, "AT+CSQ", resp, 3000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] AT+CSQ failed: %s", context, esp_err_to_name(err));
        return;
    }

    int rssi_raw = -1, ber = -1;
    char *p = strstr(resp, "+CSQ:");
    if (p) sscanf(p, "+CSQ: %d,%d", &rssi_raw, &ber);

    int rssi_dbm = (rssi_raw == 99) ? 0 : (-113 + 2 * rssi_raw);
    const char *quality;
    if (rssi_raw == 99)      quality = "unknown";
    else if (rssi_raw >= 20) quality = "excellent";
    else if (rssi_raw >= 15) quality = "good";
    else if (rssi_raw >= 10) quality = "ok";
    else if (rssi_raw >= 5)  quality = "marginal";
    else                     quality = "poor";

    ESP_LOGI(TAG, "[%s] CSQ: raw=%d (%d dBm) ber=%d -> %s, heap=%" PRIu32,
             context, rssi_raw, rssi_dbm, ber, quality, esp_get_free_heap_size());
}

static esp_err_t send_at(esp_modem_dce_t *dce, const char *cmd, int timeout_ms)
{
    char resp[256];
    memset(resp, 0, sizeof(resp));

    ESP_LOGI(TAG, "AT> %s", cmd);

    esp_err_t err = esp_modem_at(dce, cmd, resp, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AT failed for \"%s\": %s", cmd, esp_err_to_name(err));
        return err;
    }

    resp[sizeof(resp) - 1] = '\0';
    ESP_LOGI(TAG, "AT< %s", resp);
    return ESP_OK;
}

static esp_err_t wait_for_at_ready(esp_modem_dce_t *dce)
{
    const int attempts = 12;

    for (int i = 0; i < attempts; i++) {
        ESP_LOGI(TAG, "Waiting for modem AT ready... attempt %d/%d", i + 1, attempts);

        if (send_at(dce, "AT", 2000) == ESP_OK) {
            ESP_LOGI(TAG, "Modem is AT-ready");
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t ppp_connect(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Attempting PPP data mode...");

    // upewnij się że jesteśmy w command mode
    esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // sprawdź podstawowe rzeczy
    char resp[128];
    if (esp_modem_at(dce, "AT", resp, 2000) != ESP_OK) {
        ESP_LOGE(TAG, "Modem not responding to AT");
        return ESP_FAIL;
    }

    log_signal_quality(dce, "pre-connect");

    // sprawdź rejestrację
    if (esp_modem_at(dce, "AT+CREG?", resp, 2000) == ESP_OK) {
        ESP_LOGI(TAG, "CREG: %s", resp);
    }
    if (esp_modem_at(dce, "AT+CGATT?", resp, 2000) == ESP_OK) {
        ESP_LOGI(TAG, "CGATT: %s", resp);
    }

    // przejdź w DATA
    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(DATA) failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static void connection_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        esp_task_wdt_reset();

        // czekaj na sygnał: albo trzeba nawiązać, albo trzeba reconnect
        EventBits_t bits = xEventGroupWaitBits(
            event_group,
            RECONNECT_BIT,
            pdTRUE,           // clear on exit
            pdFALSE,
            pdMS_TO_TICKS(5000)
        );

        // jeśli już połączeni i nikt nie prosi o reconnect - po prostu kręcimy WDT
        if (xEventGroupGetBits(event_group) & CONNECT_BIT) {
            esp_task_wdt_reset();
            continue;
        }

        // jeśli nie ma RECONNECT_BIT i nie ma CONNECT_BIT - to startup (pierwsze podłączenie)
        // jeśli jest RECONNECT_BIT - to reconnect

        if (bits & RECONNECT_BIT) {
            g_reconnect_count++;
            ESP_LOGW(TAG, "=== RECONNECT #%d ===", g_reconnect_count);
            // backoff: 5 s przy pierwszym, potem 10, 20, 30 (cap)
            int backoff_s = 5 * (1 << (g_reconnect_count > 3 ? 3 : g_reconnect_count - 1));
            if (backoff_s > 30) backoff_s = 30;
            ESP_LOGI(TAG, "Backoff %d s before reconnect", backoff_s);
            for (int i = 0; i < backoff_s; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_task_wdt_reset();
            }
        }

        // próba podłączenia
        esp_err_t err = ppp_connect(g_dce);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ppp_connect failed, will retry");
            xEventGroupSetBits(event_group, RECONNECT_BIT);
            continue;
        }

        // czekaj na GOT_IP do 60 s
        EventBits_t got = xEventGroupWaitBits(
            event_group,
            CONNECT_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(60000)
        );

        if (got & CONNECT_BIT) {
            ESP_LOGI(TAG, "PPP up after %s", g_reconnect_count == 0 ? "startup" : "reconnect");
            // jeśli to był reconnect, zresetuj licznik po stabilnym połączeniu
            // (zrobimy to po 30 s stabilnej pracy poniżej)

            // czekaj na ewentualny LOST_IP
            // ten task zostanie obudzony przez RECONNECT_BIT z handlera
            esp_task_wdt_reset();

            // sanity: jeśli jesteśmy stable >30 s, reset licznika
            for (int i = 0; i < 30; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_task_wdt_reset();
    if (!(xEventGroupGetBits(event_group) & CONNECT_BIT)) break;
    }       
            esp_task_wdt_reset();
            if (xEventGroupGetBits(event_group) & CONNECT_BIT) {
                if (g_reconnect_count > 0) {
                    ESP_LOGI(TAG, "Stable for 30 s, resetting reconnect counter");
                    g_reconnect_count = 0;
                }
            }
        } else {
            ESP_LOGE(TAG, "PPP did not come up in 60 s, retrying");
            xEventGroupSetBits(event_group, RECONNECT_BIT);
        }
    }
}



void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    event_group = xEventGroupCreate();
    assert(event_group);

    wifi_softap_init();

    modem_power_on();
    vTaskDelay(pdMS_TO_TICKS(12000));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_event, NULL));

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    assert(esp_netif);

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(MODEM_APN);

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = MODEM_UART_TX;
    dte_config.uart_config.rx_io_num = MODEM_UART_RX;
    dte_config.uart_config.rts_io_num = MODEM_UART_RTS;
    dte_config.uart_config.cts_io_num = MODEM_UART_CTS;
    dte_config.uart_config.baud_rate = MODEM_UART_BAUD;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config.task_stack_size = 8192;
    dte_config.dte_buffer_size = 2048;

    ESP_LOGI(TAG, "Initializing esp_modem for SIM7600...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, esp_netif);
    if (!dce) {
        ESP_LOGE(TAG, "Failed to create modem DCE");
        esp_netif_destroy(esp_netif);
        return;
    }
    g_dce = dce;

    vTaskDelay(pdMS_TO_TICKS(2000));

    // task zarządzający połączeniem (startup + reconnect w jednym)
    xTaskCreate(connection_task, "conn_mgr", 6144, NULL, 6, NULL);

    // task POST telemetry
    xTaskCreate(http_post_task, "http_post", 8192, NULL, 5, NULL);

    // app_main się kończy, FreeRTOS leci dalej
}