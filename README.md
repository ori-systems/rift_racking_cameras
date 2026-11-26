# rift_racking_cameras

Firmware for a [Waveshare ESP32-P4-ETH](https://docs.waveshare.com/ESP32-P4-ETH) running ESP-IDF and FreeRTOS.

## Build and flash

The project has been built with ESP-IDF **v6.0.2** for the `esp32p4` target.

## SSH over Ethernet

The SSH user is `ori`, the password is `password`, and the board listens on the standard SSH port, `22`. Connect to the board's active Ethernet IP.

```powershell
ssh ori@192.168.70.10
```

The prompt shows the active camera ID, for example `ori@camera_0>`. Use `help` to list the configuration commands and `exit` to close the session.

```powershell
ssh ori@192.168.70.10 camera-settings 1280 720
```

The SSH host key persists across restarts. The board serves one SSH session at a time and closes idle sessions after two minutes. Commands that save network or Zenoh settings reply and then reboot the board. Reconnect using its new IP if you changed the address.

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
| `camera-zenoh` | Shows the camera ID, Zenoh router, and ROS 2 topic when Zenoh is selected. |
| `camera-zenoh id <id\|auto>` | Saves a camera ID or restores the automatic ID. |
| `camera-zenoh endpoint <tcp/IP:port>` | Saves the Zenoh router address and port. |
| `camera-zenoh topic <suffix>` | Saves the part of the ROS topic after the camera ID. |
| `camera-settings` | Shows the requested camera resolution. |
| `camera-settings <width> <height>` | Changes the camera resolution while the board is running. |

Ethernet, NTP, and Zenoh settings are saved to flash and take effect after a restart. Resolution changes take effect on the running board and return to the default in `main.c` when the board restarts.

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

The board starts asking for the time after Ethernet has an IPv4 address. To use the PC as its time source, enable the PC's NTP server and allow the board to reach it on **UDP port 123**.

For the direct cable example, enter the following over UART.

```text
ntp-server 192.168.50.1
```

After restart, wait for `Clock synced to 192.168.50.1`, then enter `time`. The output is UTC. If the board keeps waiting, ping the board first, verify the saved server address, and check that the PC is serving NTP to the board. See the [ESP-IDF time synchronization guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/system_time.html) for how the firmware's clock is set.

## Camera configuration

### Select the camera sensor

Set `CAMERA_SENSOR` in `main.c` to match the camera connected to the CSI port, then rebuild and flash. Each build includes the selected camera driver and starts at 1920×1080.

| Camera | `CAMERA_SENSOR` value | Available resolutions |
| --- | --- | --- |
| Raspberry Pi Camera (B) | `CAMERA_SENSOR_OV5647` | 800×640, 800×800, 800×1280, 1920×1080 |
| Raspberry Pi Camera Module 3 | `CAMERA_SENSOR_IMX708` (default) | 640×480, 800×600, 1024×768, 1280×720, 1920×1080 |

Camera Module 3 connects to the 22-pin CSI port with a 15-to-22-pin Raspberry Pi camera cable. The firmware uses the [IMX camera driver for ESP32-P4](https://components.espressif.com/components/mushbraindave/esp_cam_sensor_imx/versions/0.4.0/readme) with automatic exposure and white balance. Autofocus is disabled.

### Change resolution

The `.mode` in `main.c` sets the startup resolution. Use `camera-settings` on UART to switch to a resolution supported by the selected camera.

```text
camera-settings 1920 1080
```

Changing resolution briefly pauses the picture while the camera restarts capture. Frames are sent as fast as the board and Ethernet connection can handle them. The UART log reports average times for capture, encoding, and transport writes after the first 30 frames at each resolution.

## ROS 2 camera stream

The ESP32-P4 is a ROS 2 camera node on the `rmw_zenoh` network. It advertises its own node and image publisher, encodes JPEGs in hardware, and sends `sensor_msgs/msg/CompressedImage` directly over Zenoh-Pico. ROS 2 tools subscribe to the board through a standard Zenoh router.

Set `CAMERA_TRANSPORT_ZENOH` in `main.c` to `1` for Zenoh or `0` for HTTP, then rebuild and flash. Zenoh is the default. The board connects to `tcp/192.168.70.2:7447` by default. Match `ROS_DOMAIN_ID` on the PC to `CAMERA_ROS_DOMAIN_ID` in the firmware. The default domain is `0`.

### ROS 2 on a Linux host

With `ros-jazzy-rmw-zenoh-cpp` installed, start the stock router in one terminal. Set the board's endpoint to the Linux host's Ethernet address.

```bash
source /opt/ros/jazzy/setup.bash
ros2 run rmw_zenoh_cpp rmw_zenohd
```

In each subscriber terminal, select the Zenoh middleware before starting ROS tools.

```bash
source /opt/ros/jazzy/setup.bash
export RMW_IMPLEMENTATION=rmw_zenoh_cpp
export ROS_DOMAIN_ID=0
ros2 node list --no-daemon
ros2 topic list --no-daemon -t
```

### Camera name and topic

The default ID comes from the Ethernet MAC, so it stays stable when DHCP assigns a new IP. You can save a different ID, router endpoint, or topic suffix over UART. Each change restarts the board.

```text
camera-zenoh
camera-zenoh id racking_front
camera-zenoh endpoint tcp/192.168.70.2:7447
camera-zenoh topic image
```

These settings create node `/racking_front` and topic `/racking_front/image`. The topic carries `sensor_msgs/msg/CompressedImage` to keep Ethernet bandwidth low. Use `camera-zenoh id auto` to restore the MAC-based ID. Multiple cameras can use the same router with distinct IDs. The board stamps frames from its NTP clock after sync.

Boards with a saved topic suffix keep it after flashing. Run `camera-zenoh topic image` over UART to switch an existing board to the new name. To check the stream from ROS, substitute your ID in the command below.

```bash
ros2 topic echo --once --no-arr /racking_front/image sensor_msgs/msg/CompressedImage
```

### HTTP preview

With `CAMERA_TRANSPORT_ZENOH` set to `0`, use the Python JPEG viewer.

```powershell
python -m pip install opencv-python
python tools/view_camera.py 192.168.70.10
```

The viewer uses `http://192.168.70.10:8080/stream`. Press `Q` or `Esc` to close it. The firmware captures frames while the viewer is connected and frees the frame buffers when it closes.
