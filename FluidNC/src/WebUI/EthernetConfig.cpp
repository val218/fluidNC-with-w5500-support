#include "EthernetConfig.h"

#ifdef ENABLE_ETHERNET

#include <esp_eth.h>
#include <esp_eth_mac.h>
#include <esp_eth_phy.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "Ethernet";

// ── Pin configuration ─────────────────────────────────────────────────────────
#define W5500_CS_GPIO    14
#define W5500_INT_GPIO   9

// SPI clock: 8MHz is the safe maximum for shared SPI bus operation.
// 20MHz caused timing issues when switching between W5500 and SD card because
// the SPI master driver needs extra time to reconfigure clock between devices.
// W5500 max SPI clock is 80MHz but 8MHz prevents interference with SD card.
#define W5500_SPI_HZ     8000000   // was 20000000 — lowered to fix SD conflict

// ── W5500 task priority ───────────────────────────────────────────────────────
// Default ESP-IDF MAC task priority is 15 (very high). This caused the W5500
// RX task to starve SD card SPI operations during init and link negotiation.
// 5 is below most FluidNC realtime tasks but above idle.
#define W5500_TASK_PRIO  5         // was default 15 — lowered to fix SD conflict

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
    ESP_LOGI(TAG, "Init W5500 CS=IO%d INT=IO%d SPI=%dMHz prio=%d",
             W5500_CS_GPIO, W5500_INT_GPIO, W5500_SPI_HZ / 1000000, W5500_TASK_PRIO);

    // SPI bus is already initialised by FluidNC's SPIBus::init() via config.yaml.
    // Add W5500 as a new device on the existing bus.
    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits   = 1;         // W5500 SPI frame: [1-bit OP]
    devcfg.address_bits   = 7;         // W5500 SPI frame: [7-bit addr]
    devcfg.mode           = 0;         // SPI mode 0 (CPOL=0 CPHA=0)
    devcfg.clock_speed_hz = W5500_SPI_HZ;
    devcfg.queue_size     = 3;         // was 20 — large queue caused SD starvation
    devcfg.spics_io_num   = W5500_CS_GPIO;
    // cs_ena_pretrans/posttrans: give SD card time to recover after W5500 CS cycle
    devcfg.cs_ena_pretrans  = 2;       // 2 SPI clock cycles CS asserted before data
    devcfg.cs_ena_posttrans = 2;       // 2 SPI clock cycles CS held after data

    spi_device_handle_t spi_handle = NULL;
    esp_err_t err = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: 0x%x", err);
        return;
    }

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle);
    w5500_config.int_gpio_num = W5500_INT_GPIO;  // interrupt-driven (not polling)

    eth_mac_config_t mac_config   = ETH_MAC_DEFAULT_CONFIG();
    mac_config.smi_mdc_gpio_num   = -1;           // not used for SPI Ethernet
    mac_config.smi_mdio_gpio_num  = -1;
    mac_config.rx_task_stack_size = 4096;
    mac_config.rx_task_prio       = W5500_TASK_PRIO;  // was default 15, now 5

    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) { ESP_LOGE(TAG, "MAC create failed"); return; }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num   = -1;              // no external reset pin

    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) { ESP_LOGE(TAG, "PHY create failed"); return; }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_config, &s_eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Driver install failed"); return;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* eth_netif = esp_netif_new(&netif_cfg);
    esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handle));

    esp_event_handler_register(ETH_EVENT,  ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_handler, NULL);

    esp_eth_start(s_eth_handle);

    // FIX: Wait for W5500 to complete hardware reset and SPI initialisation.
    // During reset, W5500 drives MISO in an undefined state. Any SD card SPI
    // operation during this window gets corrupted, causing FatFS to unmount.
    // 500ms is enough for W5500 software reset (datasheet: ~1ms) + margin.
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "W5500 init complete");
}

#endif
