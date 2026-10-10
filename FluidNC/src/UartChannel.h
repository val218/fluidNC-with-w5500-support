// Copyright (c) 2023 -  Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Uart.h"
#include "Channel.h"
#include "lineedit.h"

class UartChannel : public Channel, public Configuration::Configurable {
private:
    Lineedit* _lineedit;
    Uart*     _uart = nullptr;

    uint32_t _uart_num           = 0;
    int32_t  _report_interval_ms = 0;
    uint32_t _last_greeting_ms   = 0;

    // Set once a complete line or an expander ID arrives.  A received byte is
    // not enough: line noise at power-up produces those.
    bool _peer_spoke = false;

    // Link-presence detection (PendantLink). Only *recognisable* peer traffic
    // counts: the pendant's status poll (XON immediately followed by '?') or a
    // complete command line. A floating/unplugged RX pin produces noise bytes,
    // which are counted separately so noise never looks like a pendant.
    volatile uint32_t _last_rx_ms   = 0;  // last recognised peer activity
    volatile uint32_t _last_tx_ms   = 0;
    volatile bool     _rx_seen      = false;
    volatile uint32_t _xon_ms       = 0;
    volatile uint32_t _pings        = 0;
    volatile uint32_t _lines        = 0;
    volatile uint32_t _noise        = 0;  // bytes that no FluidNC peer sends
    volatile uint32_t _rx_bytes     = 0;
    bool              note_rx(int c);  // true: a byte no pendant sends (noise)

    // Pendant guard (uart_channel1 only): bytes that arrive in a burst of
    // line noise - the pendant's boot ROM log at 115200 baud, a cable being
    // plugged in - are dropped instead of being taken as realtime commands
    // (feed hold, safety door, reset, macros...).
    bool              _rt_guard        = false;
    volatile uint32_t _quarantine_until = 0;
    volatile uint32_t _rx_dropped      = 0;
    bool              _trace           = false;  // echo pendant lines to the other channels

    static constexpr int _ack_timeout = 2000;

public:
    uint32_t last_rx_ms() const { return _last_rx_ms; }
    uint32_t last_tx_ms() const { return _last_tx_ms; }
    bool     rx_seen() const { return _rx_seen; }
    uint32_t pings() const { return _pings; }
    uint32_t lines() const { return _lines; }
    uint32_t noise() const { return _noise; }
    uint32_t rx_bytes() const { return _rx_bytes; }
    uint32_t rx_dropped() const { return _rx_dropped; }
    void     set_rt_guard(bool on) { _rt_guard = on; }
    void     set_trace(bool on) { _trace = on; }
    bool     trace() const { return _trace; }

    UartChannel(objnum_t num, bool addCR = false);

    void init() override;
    void init(Uart* uart);

    void handle() override;
    void queueInput(const char* buf, size_t len);
    void sendGreeting();

    // Print methods (Stream inherits from Print)
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t len) override;

    // Stream methods (Channel inherits from Stream)
    int peek(void) override;
    int available(void) override;
    int read() override;

    // Channel methods
    int      rx_buffer_available() override;
    void     flushRx() override;
    size_t   timedReadBytes(char* buffer, size_t length, TickType_t timeout) override;
    size_t   timedReadBytes(uint8_t* buffer, size_t length, TickType_t timeout) { return timedReadBytes((char*)buffer, length, timeout); };
    bool     realtimeOkay(char c) override;
    bool     lineComplete(char* line, char c) override;
    uint32_t uart_num() { return _uart_num; }
    Uart*    uart() { return _uart; }

    bool setAttr(pinnum_t index, bool* valuep, const std::string& s);

    void out(const std::string& s, const char* tag) override;
    void out_acked(const std::string& s, const char* tag) override;

    void beginJSON(const char* json_tag) override;
    void endJSON(const char* json_tag) override;

    void getExpanderId();

    void registerEvent(pinnum_t pinnum, InputPin* obj);

    // Configuration methods
    void group(Configuration::HandlerBase& handler) override {
        // @config report_interval_ms
        // @default 0
        // @default_note off
        // @tuning per-machine
        // Interval, in milliseconds, at which a status report is proactively pushed to this
        // channel, whether or not the machine is moving -- useful for driving a DRO without
        // it having to poll. Also marks the channel as one that expects to be spoken to, so
        // the startup greeting is repeated until the device answers. 0 disables proactive
        // reporting. No range is enforced by this item() call itself,
        // but keeping it at 0 or in roughly the 50-5000 range is recommended to avoid
        // overloading the processor with reports.
        handler.item("report_interval_ms", _report_interval_ms);

        // @config uart_num
        // @default 0
        // @tuning per-machine
        // Which previously-defined top-level uartN: section this channel runs over.
        handler.item("uart_num", _uart_num);

        // @config message_level
        // @default Verbose
        // @ignore_drift MsgLevelVerbose is the enum value for "Verbose", not a plain literal
        // Limits which log messages are sent to this channel, ordered from least to most
        // verbose: None < Error < Warn < Info < Debug < Verbose. Only messages at or below
        // the chosen verbosity are sent -- e.g. Info sends None/Error/Warn/Info messages but
        // holds back Debug/Verbose ones. Useful for a display/pendant that doesn't want to
        // parse messages it has no use for. The global $Message/Level setting is an
        // additional filter on top of this one; a message must pass both to reach this
        // channel.
        handler.item("message_level", _message_level, messageLevels2);
    }
};
