#include "EthernetConfig.h"

#ifdef ENABLE_ETHERNET

#include <ETH.h>
#include <driver/spi_master.h>
#include <esp_log.h>

static const char* TAG = "Ethernet";

#define W5500_CS_GPIO    14
#define W5500_INT_GPIO   9
#define W5500_RST_GPIO   -1
#define W5500_PHY_ADDR   1
#define W5500_SPI_HOST   SPI2_HOST
#define W5500_SPI_SCK    12
#define W5500_SPI_MISO   13
#define W5500_SPI_MOSI   11
#define W5500_SPI_MHZ    20

static void eth_event_handler(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ESP_LOGI(TAG, "Started");
            ETH.setHostname("fluidnc");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            ESP_LOGI(TAG, "Link Up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            ESP_LOGI(TAG, "IP: %s", ETH.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ESP_LOGI(TAG, "Link Down");
            break;
        case ARDUINO_EVENT_ETH_STOP:
            ESP_LOGI(TAG, "Stopped");
            break;
        default:
            break;
    }
}

void ethernet_init() {
    ESP_LOGI(TAG, "Init W5500 CS=IO%d INT=IO%d", W5500_CS_GPIO, W5500_INT_GPIO);

    Network.onEvent(eth_event_handler);

    ETH.begin(
        ETH_PHY_W5500,
        W5500_PHY_ADDR,
        W5500_CS_GPIO,
        W5500_INT_GPIO,
        W5500_RST_GPIO,
        W5500_SPI_HOST,
        W5500_SPI_SCK,
        W5500_SPI_MISO,
        W5500_SPI_MOSI,
        W5500_SPI_MHZ
    );

    ESP_LOGI(TAG, "W5500 init complete");
}

#endif  // ENABLE_ETHERNET
