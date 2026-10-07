// Copyright (c) 2026 Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Config.h"
#if MAX_N_ETH

#    include "EthPhy.h"
#    include "MachineConfig.h"
#    include "Assertion.h"

#    include <ETH.h>
#    include <driver/spi_master.h>
#    include <driver/gpio.h>
#    include "NutsBolts.h"  // delay_ms, to_hex

namespace Machine {
    // Read the W5500 VERSIONR register (common block, address 0x0039) directly
    // over SPI, before handing the chip to the Arduino/IDF driver.  A healthy
    // W5500 always returns 0x04.  When ETH.begin() fails it only says "init
    // failed"; this tells us whether the chip answers at all, which separates
    // wiring/power/clock problems from driver problems.
    // Returns the byte read, or -1 if the SPI transaction itself failed.
    static int w5500ReadVersion(int csPin) {
        spi_device_interface_config_t dev = {};
        dev.mode                          = 0;
        dev.clock_speed_hz                = 1000000;  // slow and forgiving for a probe
        dev.spics_io_num                  = csPin;
        dev.queue_size                    = 1;

        spi_device_handle_t handle;
        if (spi_bus_add_device(SPI2_HOST, &dev, &handle) != ESP_OK) {
            return -1;
        }
        // W5500 frame: 16-bit address, control byte (BSB=0 common, read, VDM),
        // then one data byte clocked out by the chip.
        spi_transaction_t t = {};
        t.flags             = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.length            = 32;
        t.tx_data[0]        = 0x00;
        t.tx_data[1]        = 0x39;
        t.tx_data[2]        = 0x00;
        t.tx_data[3]        = 0x00;
        esp_err_t err       = spi_device_polling_transmit(handle, &t);
        spi_bus_remove_device(handle);
        return err == ESP_OK ? int(t.rx_data[3]) : -1;
    }

    static void w5500Diagnose(int csPin, int rstPin) {
        if (rstPin >= 0) {
            // Make sure the chip is out of reset for the probe.
            gpio_set_direction(gpio_num_t(rstPin), GPIO_MODE_OUTPUT);
            gpio_set_level(gpio_num_t(rstPin), 0);
            delay_ms(2);
            gpio_set_level(gpio_num_t(rstPin), 1);
            delay_ms(60);
        }
        int v = w5500ReadVersion(csPin);
        if (v == 0x04) {
            log_info("W5500 probe: VERSIONR=0x04 (chip answers on SPI)");
        } else if (v < 0) {
            log_error("W5500 probe: SPI transaction failed (bus not initialised?)");
        } else if (v == 0x00) {
            log_error("W5500 probe: VERSIONR=0x00 - MISO held low. Chip unpowered, held in reset (RSTn low),"
                      " no 25MHz clock, or MISO shorted to GND");
        } else if (v == 0xFF) {
            log_error("W5500 probe: VERSIONR=0xFF - nothing drives MISO. Check CS/SCK/MOSI/MISO wiring and"
                      " soldering, 3.3V supply, and that RSTn is high");
        } else {
            log_error("W5500 probe: VERSIONR=0x" << to_hex(uint32_t(v)) << " (expected 0x04) - corrupted SPI:"
                      " bad joint, swapped MOSI/MISO, noise, or wrong SPI mode");
        }
    }

    const EnumItem EthPhy::phyTypes[] = {
        { EthPhy::W5500, "w5500" },
        { EthPhy::KSZ8851, "ksz8851" },
        { EthPhy::DM9051, "dm9051" },
        EnumItem(EthPhy::W5500),
    };

    static eth_phy_type_t arduinoPhyType(uint32_t phy_type) {
        switch (phy_type) {
            case EthPhy::KSZ8851:
                return ETH_PHY_KSZ8851;
            case EthPhy::DM9051:
                return ETH_PHY_DM9051;
            case EthPhy::W5500:
            default:
                return ETH_PHY_W5500;
        }
    }

    void EthPhy::validate() {
        // config->_spi->defined() cannot be checked here: SPIBus::defined()
        // only becomes true once SPIBus::init() actually runs the hardware
        // init, which happens later in the startup sequence than validate().
        // That check belongs in init(), same as SDCard does it.
        Assert(_cs.defined(), "Ethernet cs_pin must be configured");
    }

    void EthPhy::afterParse() {}

    bool EthPhy::init() {
        if (!_cs.defined()) {
            log_debug("Ethernet not configured (no cs_pin)");
            return false;
        }
        if (!config->_spi->defined()) {
            log_error("Ethernet needs SPI defined");
            return false;
        }

        log_info("Ethernet PHY " << phyTypes[_phy_type].name << " cs_pin:" << _cs.name() << " int_pin:" << _int.name()
                                 << " rst_pin:" << _rst.name());

        _cs.setAttr(Pin::Attr::Output);
        pinnum_t csPin = _cs.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);

        int intPin = -1;
        if (_int.defined()) {
            _int.setAttr(Pin::Attr::Input);
            intPin = _int.getNative(Pin::Capabilities::Input | Pin::Capabilities::Native);
        }

        int rstPin = -1;
        if (_rst.defined()) {
            _rst.setAttr(Pin::Attr::Output);
            rstPin = _rst.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        }

        // config->_spi holds Pin objects for the shared SPI bus (sck/mosi/miso).
        // This must be the SAME host that spi_init_bus() used (SPI2_HOST on
        // ESP32-S3, see esp32/spi.cpp), not a separate one: two hosts cannot
        // drive the same GPIOs.  SPIBus::init() runs before modules, so the
        // bus already exists; ETH.begin()'s spi_bus_initialize() then returns
        // ESP_ERR_INVALID_STATE, which Arduino-ESP32 3.x treats as "already
        // initialized" and simply adds the W5500 as another device on the bus,
        // alongside the SD card, with the IDF bus lock arbitrating.
        pinnum_t sckPin  = config->_spi->_sck.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pinnum_t mosiPin = config->_spi->_mosi.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pinnum_t misoPin = config->_spi->_miso.getNative(Pin::Capabilities::Input | Pin::Capabilities::Native);

        if (_phy_type == W5500) {
            w5500Diagnose(int(csPin), rstPin);
        }

        bool ok = ETH.begin(arduinoPhyType(_phy_type),
                            _phy_addr,
                            int(csPin),
                            intPin,
                            rstPin,
                            SPI2_HOST,
                            int(sckPin),
                            int(misoPin),
                            int(mosiPin),
                            uint8_t(_frequency_hz / 1000000));
        if (!ok) {
            log_error("Ethernet PHY init failed");
            return false;
        }
        config_ok = true;
        return true;
    }
}
#endif
