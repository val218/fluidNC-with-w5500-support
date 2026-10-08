// PendantLink.cpp - TabUI pendant connection detection (see PendantLink.h)
#include "PendantLink.h"
#include "Machine/MachineConfig.h"
#include "UartChannel.h"
#include "Serial.h"

#include <Arduino.h>  // millis()
#include <cstdio>

namespace {
    enum class Link : uint8_t { None, Disconnected, Connected };

    // Pendant pings every 500 ms when it hears nothing; allow for a missed ping or two.
    constexpr uint32_t rx_fresh_ms   = 3000;
    // We count as quiet once nothing was sent for this long; a connected pendant
    // would have pinged inside that window.
    constexpr uint32_t tx_quiet_ms   = 1500;
    constexpr uint32_t poll_every_ms = 250;

    Link     _link        = Link::None;
    uint32_t _last_poll   = 0;
    bool     _initialized = false;

    Link evaluate(UartChannel* ch) {
        if (!ch) {
            return Link::None;
        }
        if (!ch->rx_seen()) {
            return Link::Disconnected;
        }
        uint32_t now    = millis();
        uint32_t rx_age = now - ch->last_rx_ms();
        uint32_t tx_age = now - ch->last_tx_ms();
        if (rx_age < rx_fresh_ms) {
            return Link::Connected;
        }
        if (tx_age > tx_quiet_ms) {
            return Link::Disconnected;
        }
        return _link == Link::None ? Link::Disconnected : _link;  // streaming: keep verdict
    }

    const char* name(Link l) {
        switch (l) {
            case Link::Connected:
                return "connected";
            case Link::Disconnected:
                return "disconnected";
            default:
                return "none";
        }
    }
}

UartChannel* pendant_channel() {
    if (!config) {
        return nullptr;
    }
    UartChannel* ch = config->_uart_channels[1];
    if (!ch || !ch->uart() || !ch->uart()->configured()) {
        return nullptr;
    }
    return ch;
}

const char* pendant_state_name() {
    return name(_link);
}

void pendant_report(Channel& out) {
    char buf[48];
    snprintf(buf, sizeof(buf), "[MSG:Pendant:%s]\n", name(_link));
    out.print(buf);
}

void pendant_poll() {
    uint32_t now = millis();
    if (_initialized && (now - _last_poll) < poll_every_ms) {
        return;
    }
    _last_poll   = now;
    UartChannel* ch = pendant_channel();
    Link         l  = evaluate(ch);
    if (!_initialized) {
        _initialized = true;
        _link        = l;
        return;
    }
    if (l != _link) {
        _link = l;
        char buf[48];
        snprintf(buf, sizeof(buf), "[MSG:Pendant:%s]\r\n", name(l));
        allChannels.print_except(buf, ch);
    }
}
