# W5500 Ethernet on FluidNC 4.1.1 (ESP32-S3)

This branch (`fluidnc-4.1.1-w5500`) is stock FluidNC v4.1.1 plus fixes so that
everything works when the network is Ethernet instead of WiFi:

- WebUI / ESP800 / ESP111 report the Ethernet IP (was `0.0.0.0`)
- hostname (`fluidnc`) is used for DHCP, mDNS, OTA and ESP800
- mDNS (`http://fluidnc.local`), OTA and notifications start on Ethernet
- `$Ethernet/Setup=IP=... MSK=... GW=...` and `$Sta/Setup=...` parse correctly

Tested on a dpCREATOR R2 (ESP32-S3-WROOM-1-N16R8) with an external W5500 module.

---

## 1. Get the firmware

Firmware is built automatically by GitHub Actions on every push to this branch.

1. Open the **Actions** tab of this repository.
2. Open the latest run of **"Build FluidNC 4.1.1 W5500 (ESP32-S3)"**.
3. Download the artifact zip at the bottom of the run page.

| File | Use |
|---|---|
| `merged-flash.bin` | Fresh board. Bootloader + partitions + firmware + filesystem, flash at `0x0`. **Overwrites `config.yaml` with the default — back yours up first.** |
| `firmware.bin` | Board already running FluidNC. Flash at `0x10000` or upload via WebUI. Keeps config and settings. |
| `firmware.elf` | Debug symbols, only needed to decode a crash backtrace. |

```
esptool.py --chip esp32s3 write_flash 0x0 merged-flash.bin
# or, keep existing config:
esptool.py --chip esp32s3 write_flash 0x10000 firmware.bin
```

To rebuild without changing code: **Actions → Build FluidNC 4.1.1 W5500 → Run workflow**.

---

## 2. Wiring

The W5500 shares the SPI bus with the SD card.

| W5500 module | ESP32-S3 | Notes |
|---|---|---|
| SCLK | GPIO12 | shared with SD card |
| MOSI | GPIO11 | shared with SD card |
| MISO | GPIO13 | shared with SD card |
| CS (SCSn) | GPIO14 | |
| INT | GPIO9 | optional — leave `int_pin: NO_PIN` if not connected |
| RST | — | optional, not used |
| 3V3 / GND | 3V3 / GND | short wires, solid ground |

SD card CS is GPIO10.

> **Important:** if INT is configured but not physically connected, the board gets
> link-up but never receives a packet (DHCP fails, ping times out). Either wire INT
> or set `int_pin: NO_PIN` (polling mode, ~10 ms extra latency, fine for CNC).

---

## 3. config.yaml

Add / adjust these sections (pins as above):

```yaml
spi:
  miso_pin: gpio.13
  mosi_pin: gpio.11
  sck_pin: gpio.12

sdcard:
  cs_pin: gpio.10
  card_detect_pin: NO_PIN
  frequency_hz: 8000000

ethernet:
  cs_pin: gpio.14
  int_pin: NO_PIN        # gpio.9 if the module's INT is wired
  rst_pin: NO_PIN
  phy_type: w5500
  phy_addr: 1
  frequency_hz: 8000000  # 8 MHz is safe on a bus shared with the SD card
```

Upload it (WebUI file manager or `$LocalFS`), then reboot with `$bye`.

---

## 4. Switch FluidNC to Ethernet

Optional first test, while still on WiFi — brings up only the W5500:

```
$Ethernet/Init
$Ethernet/Status
```

Expect `Ethernet PHY init succeeded` and `Link: Up` with a cable plugged in.

Switch the network to Ethernet:

```
$network/type=Ethernet
$bye
```

WiFi (including the AP) is off after this. To go back: `$network/type=WiFi` over USB, then `$bye`.

### DHCP (default)

```
$Ethernet/IPMode=DHCP
$bye
```

Recommended: add a **DHCP reservation** in your router for the board's MAC
(shown in `$Ethernet/Status`) so the IP never changes — gSender needs a fixed IP.

### Static IP

Use an address in **your** LAN subnet (check your PC with `ipconfig`):

```
$Ethernet/IPMode=Static
$Ethernet/IP=192.168.10.50
$Ethernet/Netmask=255.255.255.0
$Ethernet/Gateway=192.168.10.1
$bye
```

or in one line:

```
$Ethernet/Setup=IP=192.168.10.50 MSK=255.255.255.0 GW=192.168.10.1
```

### Check it

`$Startup/Show` should contain:

```
[MSG:INFO: Ethernet link up]
[MSG:INFO: Ethernet IP is 192.168.10.x]
[MSG:INFO: Start mDNS with hostname:http://fluidnc.local/]
[MSG:INFO: HTTP started on port 80]
[MSG:INFO: Telnet started on port 23]
```

From the PC:

```
ping 192.168.10.x
```

Then open `http://192.168.10.x` or `http://fluidnc.local` for the WebUI.

---

## 5. Connect gSender over Ethernet

gSender connects to FluidNC's **Telnet server** (raw TCP). Tested with gSender 1.6.4.

1. Find the board's IP (`$Ethernet/Status` or the boot log) and Telnet port
   (`$Telnet/Port`, default `23`).
2. In gSender open **Config → Ethernet**:
   - **Connect to IP**: the board's IP, e.g. `192.168.10.102`
   - **Ethernet port**: same as `$Telnet/Port` (default `23`)
3. Click **Apply Settings**.
4. Open the connection menu (top left) and click the **Ethernet** entry
   (`192.168.10.x — Ethernet (port N)`), not a COM port.

> **Do not use gSender's "Remote Mode" for this.** Remote Mode is a web server that
> runs on the PC so phones/tablets can control gSender; its IP must be the **PC's own**
> address. Putting the controller IP there gives
> *"There was a problem connecting to the remote address… Remote mode has been disabled"*.

Changing the Telnet port on the board (if needed):

```
$Telnet/Port=2323
$bye
```

…and set the same number in gSender's Ethernet port field.

---

## 6. SD card as a network drive (Windows)

The board serves its SD card over the LAN with WebDAV at `http://<board-ip>/sd`.
Windows can map that to a drive letter, which then shows up in **This PC** and
reconnects at every sign-in. Copy G-code onto it like onto a USB stick.

**One-time setup** (Command Prompt **as administrator**):

```
sc config WebClient start= auto
net start WebClient
reg add HKLM\SYSTEM\CurrentControlSet\Services\WebClient\Parameters /v FileSizeLimitInBytes /t REG_DWORD /d 4294967295 /f
net stop WebClient && net start WebClient
```

The `WebClient` service is the Windows WebDAV client. The registry line lifts
its default 50 MB per-file limit (large G-code files fail to copy without it).

**Map the drive** (normal Command Prompt, use your board's IP):

```
net use S: http://192.168.10.104/sd /persistent:yes
```

or in Explorer: **This PC → Map network drive**, Folder `http://192.168.10.104/sd`,
tick **Reconnect at sign-in**.

Give the board a fixed address (static IP, or a DHCP reservation on the router
for its MAC) so the drive keeps working after a reboot.

**Pendant previews are built automatically.** Whenever a G-code file
(`.nc .gcode .gc .ngc .tap .cnc .g`) is written to the SD card, from this
drive, the web UI or the classic UI, FluidNC queues `<file>.viz` for the TabUI
pendant. The build waits until the machine is **Idle with no job running**
(it pauses the controller for a moment), starts 1.5 s after the last write,
and replaces any old `.viz`. Deleting or renaming a G-code file over the drive
removes its `.viz`. You will see the `.viz` files next to your G-code in
Explorer; leave them there. Progress shows in the web UI (and any console) as
`[MSG:VizAutoBusy/VizAutoReady/VizAutoErr:...]`; these are not sent to the
pendant, so they never replace what the pendant is showing.
`$Viz/Refresh=/sd/file.nc` queues a rebuild by hand.

Tips:
- Don't copy big files while a job is running from the SD card; the card is
  shared and the job has priority. The `.viz` build waits for the job anyway.
- If Explorer is slow to open the drive, untick **Automatically detect
  settings** in Internet Options → Connections → LAN settings (WebClient
  waits for proxy auto-detection).
- macOS: Finder → Go → Connect to Server → `http://192.168.10.104/sd`.
  Linux: `davfs2` or your file manager's `dav://192.168.10.104/sd`.

## 7. Pendant connection status

`$Pendant/Status` answers `[MSG:Pendant:connected]`, `disconnected` or `none`
(no `uart_channel1` configured), and every channel except the pendant's own
gets that message when it changes. The web UI shows it as a badge in the
header. Detection uses the pendant's link pings: an idle pendant sends `?`
every 500 ms, so an unplugged pendant is noticed within about 3 s while the
machine is idle. During a running job the board streams reports and the
pendant may stay quiet, so the badge keeps its last state until the job ends.

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Ethernet PHY init failed` | Wrong SPI/CS pins, wiring, or no `spi:` section. |
| Link up, IP `0.0.0.0` (DHCP), ping fails | INT configured but not wired → set `int_pin: NO_PIN`. Or no DHCP server on that port. |
| Static IP set, ping says *Destination host unreachable* | Board and PC in different subnets, or board now has a different IP (DHCP). |
| `Test-NetConnection <ip> -Port <port>` → `TcpTestSucceeded : False` but ping OK | Telnet port differs from what you test; check `$Telnet/Port`, `$Telnet/Enable=ON`. |
| gSender: *"Remote mode has been disabled"* | Controller IP was entered in Remote Mode. Use **Config → Ethernet** instead. |
| gSender: *Unable to connect* | IP/port in Config → Ethernet don't match the board; click **Apply Settings**. |
| Boot shows *"Showing startup log from previous panic"* | Capture serial output at 115200 including `Guru Meditation` and `Backtrace:` lines and decode via **Actions → Run workflow** with the backtrace input. |

Windows has no telnet client by default; test the port with PowerShell:

```powershell
Test-NetConnection 192.168.10.102 -Port 23
```

or use PuTTY (connection type **Raw**), type `?` + Enter → `<Idle|MPos:...>`.

### Useful commands

```
$Ethernet/Status        link, IP, mask, gateway, MAC
$Ethernet/Setup         show static IP settings
$Ethernet/Init          bring up the W5500 manually (test)
$network/type           WiFi | Ethernet
$Telnet/Port            Telnet (gSender) port
[ESP800]json=yes        WebSocketIP should be the Ethernet IP
```
