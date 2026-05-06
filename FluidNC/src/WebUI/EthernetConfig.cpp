#include "EthernetConfig.h"

#ifdef ENABLE_ETHERNET

#include <esp_eth.h>
#include <esp_eth_mac.h>
#include <esp_eth_phy.h>
#include <driver/spi_master.h>
#include <esp_netif.h>
#include <esp_event.h>
#include "../Logging.h"

// dpCREATOR R2 - confirmed from schematic
#define W5500_CS_GPIO   14
#define W5500_INT_GPIO  9
#define W5500_SPI_HOST  SPI2_HOST  // shared with SD (MOSI=11,MISO=13,SCK=12)
#define W5500_SPI_HZ    20000000   // 20 MHz conservative on shared bus

static esp_eth_handle_t s_eth_handle = NULL;

static void eth_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            log_info("Ethernet: Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            log_info("Ethernet: Link Down");
            break;
        case ETHERNET_EVENT_START:
            log_info("Ethernet: Started");
            break;
        case ETHERNET_EVENT_STOP:
            log_info("Ethernet: Stopped");
            break;
        default:
            break;
    }
}

static void got_ip_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    log_info("Ethernet IP: " IPSTR, IP2STR(&event->ip_info.ip));
    log_info("Ethernet GW: " IPSTR, IP2STR(&event->ip_info.gw));
}

void ethernet_init() {
    log_info("Ethernet: Init W5500 CS=IO14 INT=IO9");

    // IMPORTANT: Do NOT call spi_bus_initialize() here.
    // FluidNC's SD card driver already owns SPI2_HOST.
    // We add W5500 as a second device on the existing bus.

    spi_device_interface_config_t spi_devcfg = {};
    spi_devcfg.mode           = 0;
    spi_devcfg.clock_speed_hz = W5500_SPI_HZ;
    spi_devcfg.queue_size     = 20;
    spi_devcfg.spics_io_num   = W5500_CS_GPIO;

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = W5500_INT_GPIO;

    eth_mac_config_t mac_config   = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        log_error("Ethernet: W5500 MAC create failed");
        return;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num   = -1;  // no RST pin on dpCREATOR R2

    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        log_error("Ethernet: W5500 PHY create failed");
        return;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) {
        log_error("Ethernet: driver install failed (%s)", esp_err_to_name(err));
        return;
    }

    // Attach to lwIP TCP/IP stack
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* eth_netif       = esp_netif_new(&netif_cfg);
    esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handle));

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                &eth_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                &got_ip_handler, NULL);

    esp_eth_start(s_eth_handle);
    log_info("Ethernet: W5500 init complete");
}

#endif  // ENABLE_ETHERNET
