# rift_racking_cameras

Firmware for a [Waveshare ESP32-P4-ETH](https://docs.waveshare.com/ESP32-P4-ETH) running ESP-IDF and FreeRTOS.

## Build and flash

The project has been built with ESP-IDF **v6.0.2** for the `esp32p4` target.

## UART commands

Open the monitor at **115200 baud** and enter commands at the `rift>` prompt.

| Command | What it does |
| --- | --- |
| `help` | Lists available commands. |
| `net-static <ip> <netmask> [gateway]` | Saves a fixed IPv4 address, mask, and optional gateway. |
| `net-dhcp` | Clears the fixed address and restores DHCP. |
| `ntp-server <pc-ip>` | Saves the IPv4 address of the NTP server. |
| `ntp-server auto` | Uses the NTP server supplied by DHCP option 42. |
| `ntp-server off` | Turns off clock sync. |
| `time` | Shows the current UTC time after sync, or reports that the clock is still waiting. |

Commands that change a setting save it to flash and restart the board. The settings survive power cycles.

## Ethernet and ping

### LAN with DHCP

DHCP is the default. Connect the board's RJ45 port to the same LAN as the PC. On UART, look for `Link up` and the active `IPv4` address. From the PC, replace the example IP below with the address shown on UART.

```powershell
ping 192.168.1.50
```

On a routed LAN, use an available address on that LAN if you want to set a fixed IP. Supply the router's address as the gateway when the board needs to reach other networks.

### Direct cable to the PC

For an isolated cable between the PC and the board, give them different addresses on the same subnet. This example uses PC address `192.168.50.1` and board address `192.168.50.2`, both with mask `255.255.255.0`. Use a subnet that is free on your PC.

1. Set the PC's Ethernet adapter to `192.168.50.1` with mask `255.255.255.0`.
2. On UART, run `net-static 192.168.50.2 255.255.255.0`. The gateway can be left out for this connection.
3. After restart and `Link up`, run `ping 192.168.50.2` on the PC.

A working link with no DHCP address usually means the network lacks a reachable DHCP server. Check the router or use fixed addresses for a direct cable.

## Sync with the PC clock

The board defaults to automatic NTP discovery when no server setting is saved. The DHCP server must advertise an NTP address in option 42. When it does, the board logs `NTP server from DHCP: <ip>`. A saved server address takes precedence. Use `ntp-server auto` to switch back to discovery. On a direct cable with a fixed IP, set the server address manually.

The board starts asking for the time after Ethernet has an IPv4 address. To use the PC as its time source, enable the PC's NTP server and allow the board to reach it on **UDP port 123**. On Windows, see the [Windows Time service settings](https://learn.microsoft.com/en-us/windows-server/networking/windows-time-service/windows-time-service-tools-and-settings?tabs=config) for the `NtpServer` provider.

For the direct cable example, enter the following over UART.

```text
ntp-server 192.168.50.1
```

After restart, wait for `Clock synced to 192.168.50.1`, then enter `time`. The output is UTC and ends with `Z`. If the board keeps waiting, ping the board first, verify the saved server address, and check that the PC is serving NTP to the board. See the [ESP-IDF time synchronization guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/system_time.html) for how the firmware's clock is set.
