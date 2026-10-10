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
    uint32_t _last_noise  = 0;  // noise counter at the previous poll
    uint32_t _noisy_until = 0;  // treat the link as down until then

    Link evaluate(UartChannel* ch) {
        if (!ch) {
            return Link::None;
        }
        if (!ch->rx_seen()) {
            return Link::Disconnected;
        }
        uint32_t now = millis();
        // Garbage on the line (floating RX pin, wrong baud, unplugged cable)
        // is never a working pendant.
        uint32_t noise = ch->noise();
        if (noise - _last_noise > 3) {
            _noisy_until = now + rx_fresh_ms;
        }
        _last_noise = noise;
        if ((int32_t)(_noisy_until - now) > 0) {
            return Link::Disconnected;
        }
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

void pendant_debug(Channel& out) {
    UartChannel* ch = pendant_channel();
    char         buf[200];
    if (!ch) {
        out.print("[MSG:INFO: Pendant: no uart_channel1 configured]\n");
        return;
    }
    uint32_t now = millis();
    snprintf(buf, sizeof(buf),
             "[MSG:INFO: Pendant %s: rx bytes %u, polls %u, lines %u, noise bytes %u, dropped %u, trace %s, last activity %s%.1f s ago, last tx %.1f s ago]\n",
             name(_link), (unsigned)ch->rx_bytes(), (unsigned)ch->pings(), (unsigned)ch->lines(), (unsigned)ch->noise(),
             (unsigned)ch->rx_dropped(), ch->trace() ? "on" : "off",
             ch->rx_seen() ? "" : "never/", ch->rx_seen() ? (now - ch->last_rx_ms()) / 1000.0f : 0.0f,
             (now - ch->last_tx_ms()) / 1000.0f);
    out.print(buf);
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
    if (ch) {
        ch->set_rt_guard(true);  // line-noise bursts never become realtime commands
    }
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

void pendant_trace(Channel& out, bool on) {
    UartChannel* ch = pendant_channel();
    if (ch) {
        ch->set_trace(on);
    }
    out.print(on ? "[MSG:INFO: Pendant trace on: its command lines are echoed as PND>]\n" : "[MSG:INFO: Pendant trace off]\n");
}
