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

#include "esp_http_client.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"

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

static const char *TAG = "lte_pppos";

static EventGroupHandle_t event_group;
static const int CONNECT_BIT = BIT0;
static const int STOP_BIT    = BIT1;


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
    // chwila na ustabilizowanie route'a po PPP
    vTaskDelay(pdMS_TO_TICKS(1000));

    const char *url = "http://httpbin.org/post";

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .keep_alive_enable = true,   // <-- ważne dla utrzymania TCP
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    int counter = 0;
    char payload[96];

    while (1) {
        // budujemy świeży payload za każdym razem
        snprintf(payload, sizeof(payload),
                 "{\"device\":\"esp32-sim7600\",\"value\":%d}", counter++);

        esp_http_client_set_url(client, url);            // można pominąć jeśli URL się nie zmienia
        esp_http_client_set_post_field(client, payload, strlen(payload));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "POST OK, status=%d len=%" PRId64,
                     esp_http_client_get_status_code(client),
                     esp_http_client_get_content_length(client));
        } else {
            ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(10000));   // co 10 s
    }

    // tu nigdy nie dochodzimy, ale dla porządku:
    esp_http_client_cleanup(client);
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

        // PPP jako domyślny interfejs do internetu
        esp_netif_set_default_netif(netif);

        // Pokaż jaki DNS dał operator
        esp_netif_dns_info_t dns_main = {0};
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main);
        ESP_LOGI(TAG, "DNS main: " IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));

        // Fallback: jeśli operator nie dał DNS, wymuś Google DNS
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
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP LOST IP");
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

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    event_group = xEventGroupCreate();
    assert(event_group);

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
    dte_config.task_stack_size = 4096;
    dte_config.dte_buffer_size = 1024;

    ESP_LOGI(TAG, "Initializing esp_modem for SIM7600...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, esp_netif);
    if (!dce) {
        ESP_LOGE(TAG, "Failed to create modem DCE");
        esp_netif_destroy(esp_netif);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Starting AT debug sequence...");

    if (wait_for_at_ready(dce) != ESP_OK) {
        ESP_LOGE(TAG, "Modem does not respond to AT after retries");
        goto cleanup;
    }

    if (send_at(dce, "ATI", 5000) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read ATI");
    }

    if (send_at(dce, "AT+CSQ", 5000) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read CSQ");
    }

    if (send_at(dce, "AT+CREG?", 5000) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read CREG");
    }

    if (send_at(dce, "AT+CGREG?", 5000) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read CGREG");
    }

    if (send_at(dce, "AT+CGATT?", 5000) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read CGATT");
    }

    ESP_LOGI(TAG, "Trying to switch modem to data mode...");
    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set data mode: %s", esp_err_to_name(err));
        goto cleanup;
    }

    EventBits_t bits = xEventGroupWaitBits(
        event_group,
        CONNECT_BIT | STOP_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(60000)
    );

    if (bits & CONNECT_BIT) {
    ESP_LOGI(TAG, "PPP connected, launching HTTP POST task");
    xTaskCreate(http_post_task, "http_post", 6144, NULL, 5, NULL);
    } else if (bits & STOP_BIT) {
        ESP_LOGE(TAG, "PPP stopped by user/error");
    } else {
        ESP_LOGE(TAG, "PPP connect timeout");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

cleanup:
    esp_modem_destroy(dce);
    esp_netif_destroy(esp_netif);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}