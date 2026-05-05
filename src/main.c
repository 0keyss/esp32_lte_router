#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "nvs_flash.h"

#include "esp_modem_api.h"
#include "esp_modem_config.h"
#include "esp_modem_dce_config.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"


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
static const int STOP_BIT = BIT1;


static void http_test_task(void *pv)
{
    const char *host = "example.org";
    const char *port = "80";
    const char *request =
        "GET / HTTP/1.1\r\n"
        "Host: example.org\r\n"
        "Connection: close\r\n"
        "\r\n";

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    ESP_LOGI(TAG, "Resolving %s...", host);
    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d", err);
        vTaskDelete(NULL);
        return;
    }

    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "Failed to allocate socket");
        freeaddrinfo(res);
        vTaskDelete(NULL);
        return;
    }

    err = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (err != 0) {
        ESP_LOGE(TAG, "Socket connect failed err=%d", err);
        close(s);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Connected, sending HTTP GET...");
    if (write(s, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Write failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }

    char buf[256];
    int r;
    while ((r = read(s, buf, sizeof(buf) - 1)) > 0) {
        buf[r] = 0;
        ESP_LOGI(TAG, "HTTP chunk:\n%s", buf);
    }

    ESP_LOGI(TAG, "HTTP test done");
    close(s);
    vTaskDelete(NULL);
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

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    event_group = xEventGroupCreate();

    modem_power_on();
    vTaskDelay(pdMS_TO_TICKS(5000));

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
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    int rssi = 0, ber = 0;
    if (esp_modem_get_signal_quality(dce, &rssi, &ber) == ESP_OK) {
        ESP_LOGI(TAG, "Signal quality: rssi=%d ber=%d", rssi, ber);
    } else {
        ESP_LOGW(TAG, "Cannot read signal quality");
    }

    ESP_LOGI(TAG, "Switching modem to data mode...");
    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set data mode: %s", esp_err_to_name(err));
        esp_modem_destroy(dce);
        esp_netif_destroy(esp_netif);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        event_group,
        CONNECT_BIT | STOP_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(60000)
    );

    if (bits & CONNECT_BIT) {
        ESP_LOGI(TAG, "PPP connected, starting HTTP test...");
        xTaskCreate(http_test_task, "http_test", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG, "PPP connect timeout");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}