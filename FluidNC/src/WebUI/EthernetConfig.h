#pragma once
// W5500 Ethernet for dpCREATOR R2
// CS=IO14, INT=IO9, SPI2_HOST shared with SD card

#ifdef ENABLE_ETHERNET
void ethernet_init();
#endif