#include "EthernetConfig.h"

#ifdef ENABLE_ETHERNET

#include <esp_eth.h>
#include <esp_eth_mac.h>
#include <esp_eth_phy.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <driver/spi_master.h>
#include <esp_log.h>

static const char* TAG = "Ethernet";

#define W5500_CS_GPIO    14
#define W5500_INT_GPIO   9
#define W5500_SPI_HOST   SPI2_HOST
#define W5500_SPI_HZ     20000000

static esp_eth_handle_t s_eth_handle = NULL;

static void eth_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "Link Up");   break;
        case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "Link Down"); break;
        case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "Started");   break;
        case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "Stopped");   break;
        default: break;
    }
}

static void got_ip_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
}

void ethernet_init() {
    ESP_LOGI(TAG, "Init W5500 CS=IO%d INT=IO%d", W5500_CS_GPIO, W5500_INT_GPIO);

    // SPI bus already initialised by FluidNC SD card code.
    // Create a device handle on the existing bus.
    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits     = 1;
    devcfg.address_bits     = 7;
    devcfg.mode             = 0;
    devcfg.clock_speed_hz   = W5500_SPI_HZ;
    devcfg.queue_size       = 20;
    devcfg.spics_io_num     = W5500_CS_GPIO;

    spi_device_handle_t spi_handle = NULL;
    if (spi_bus_add_device(W5500_SPI_HOST, &devcfg, &spi_handle) != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed"); return;
    }

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle);
    w5500_config.int_gpio_num = W5500_INT_GPIO;

    eth_mac_config_t mac_config   = ETH_MAC_DEFAULT_CONFIG();
    mac_config.smi_mdc_gpio_num   = -1;
    mac_config.smi_mdio_gpio_num  = -1;
    mac_config.rx_task_stack_size = 4096;

    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) { ESP_LOGE(TAG, "MAC create failed"); return; }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num   = -1;

    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) { ESP_LOGE(TAG, "PHY create failed"); return; }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_config, &s_eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Driver install failed"); return;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* eth_netif = esp_netif_new(&netif_cfg);
    esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handle));

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_handler, NULL);

    esp_eth_start(s_eth_handle);
    ESP_LOGI(TAG, "W5500 init complete");
}

#endif
