// PendantLink.h - is the TabUI pendant on uart_channel1 connected?
//
// The pendant (val218/FluidNc-SmartTabUI) sends XON + '?' whenever it has
// received nothing for 500 ms. FluidNC only auto-reports on change, so an idle
// machine with a pendant attached sees that ping twice a second. Silence from
// the pendant while we were quiet too therefore means it is gone. While we are
// streaming reports (a job running) the pendant need not talk, so the last
// verdict is kept until the link goes quiet again.
//
//   $Pendant/Status        -> [MSG:Pendant:connected|disconnected|none]
//   on change (all channels except the pendant itself) -> same message
#pragma once
#include "Channel.h"

class UartChannel;

UartChannel* pendant_channel();          // uart_channel1 if configured, else nullptr
void         pendant_poll();             // call often from the polling task
void         pendant_report(Channel& out);
void         pendant_debug(Channel& out);  // $Pendant/Debug: raw counters
void         pendant_trace(Channel& out, bool on);  // $Pendant/Trace=on|off
const char*  pendant_state_name();
