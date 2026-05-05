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

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "PPP GOT IP");
        ESP_LOGI(TAG, "IP      : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "NETMASK : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "GW      : " IPSTR, IP2STR(&event->ip_info.gw));
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
        ESP_LOGI(TAG, "PPP connected");
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