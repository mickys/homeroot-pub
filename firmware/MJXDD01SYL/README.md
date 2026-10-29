# MJXDD01SYL

Firmware for the Yeelight Mi Smart LED Ceiling Light 450. The hardware is `MJXDD01SYL` (`yeelink.light.ceiling22`), an ESP32.

Built with ESPHome 2026.10.1. `min_version` is 2026.1.0.

## What it runs

The ceiling is a cold/warm bulb, 2700 K to 6000 K. The night channel is a second bulb. Turning one on turns the other off. Off cuts the LEDs: `zero_means_zero` keeps a 0 level at 0, the driver enable on GPIO22 drops as soon as every channel is 0, and the default transition is 0 seconds.

HomeKit listens on port 32042. The bridge is a new accessory. A stock HomeKit pairing does not carry over. Firmware revision stays `0.1.1`. The button "Reset HomeKit pairings" clears HomeKit pairings only.

Restore after power loss is a switch on the ceiling bulb. `restore_after_power_loss` in the lamp yaml is the value used until that switch has been written. The stored switch wins after that. Both channels use `RESTORE_DEFAULT_OFF`, so a saved on-state is applied only while the switch is on.

`initial_state` brightness and color temperature apply only when the lamp has no saved level. They do not turn a light on. The ceiling range is 167 to 370 mireds. 175 mireds is about 5700 K.

A Yeelight BLE dimmer can drive the lamp. Product 950 is the YLKG07YL / YLKG08YL. A click toggles the active channel. A double click swaps the ceiling and the night channel, and if both are off it only selects the other channel. Rotation dims. Dimming does not turn the channel off. Long-press and pressed-rotate are ignored. The pair window stays closed at boot. `pair_remote` or the HomeKit switch "Pair remote" opens it for 60 seconds. `set_remote` stores a MAC, a 12-byte key, and an optional product id (`0` keeps the current product). The lamp answers that MAC only when the key decrypts the advert. The key is not written into the yaml and is not logged.

The BLE relay forwards advertisements to one LAN address as authenticated UDP. The UDP source address is the lamp that forwarded the advertisement. Leave the receiver block out and the relay stays off. `enabled: true` is the cold-boot default that turns it on. A receiver stored on the lamp replaces that default. `set_ble_gateway` stores the host, port, and enable flag. `set_ble_gateway_key` stores the 16-byte key. Without a key the relay does not send.

The fallback AP is `<node_name> Fallback`. Its captive portal uses port 80. Wi-Fi uses DHCP. The vendor MAC CRC is ignored, so the address the lamp had on stock firmware can change.

Supply sense is the raw ADC on GPIO36 at 12 dB. No scale is applied.

The ESPHome API is plaintext. OTA has a password. API actions:

| Action | What it does |
| --- | --- |
| `dump_flash`, `dump_otadata`, `log_slots` | Read-only flash slot dump |
| `redump_config` | Log the running config |
| `pair_remote`, `stop_remote_pairing` | Open or close the 60 second dimmer window |
| `set_remote` | Store `mac`, `key`, and optional `product_id`. A zero MAC or a short key is rejected |
| `set_ble_gateway` | Store `host`, `port`, and `enable` |
| `set_ble_gateway_key` | Store the 16-byte relay key as 32 hex digits |

A value already stored on the lamp wins over the yaml: the restore switch, the saved light level, the remote, and the receiver. A stored record survives an OTA update.

## Findings

### The 15 minute flicker

ESPHome reboots a device when no native API client has been connected for 15 minutes. The log line is `No clients; rebooting`. HomeKit does not hold that connection, so a HomeKit-only lamp restarts on that timer. The bulbs drop for about two seconds, then restore-after-power-loss brings the saved level back. A controller that polls slowly often records no on/off, and a wall switch that mirrors only explicit on and off edges does not click. Free heap at boot stays normal. This is the API timer, not an out-of-memory reset. This package sets `api.reboot_timeout` to `0s`. The Wi-Fi reboot timer stays at its default and fires only while Wi-Fi is down.

### Detach a dimmer with set_remote

`set_remote` replaces the stored remote. It does not clear one. The MAC is 6 bytes, 12 hex digits, with optional `:` or `-`. The call is stored only when that MAC is nonzero and the key is exactly 12 bytes. `000000000001` meets that rule. After it is stored the lamp answers only that address, so the real dimmer no longer matches. `000000000000`, a shorter string, or a key that is not 12 bytes is rejected and the previous record stays. `product_id` 0 keeps the current product. The key does not have to be the dimmer's key when the MAC is not the dimmer. Changing `remote_mac` in yaml does not move a remote that is already stored.

### A slow HomeKit button with a normal signal

The BLE scan runs continuously. `interval` and `window` are both 320 ms, so a dimmer press is not missed. That scan shares the single radio with Wi-Fi. Lamps on the same access point can show similar signal strength while one of them drops the first TCP packet to HomeKit port 32042. The sender retries about a second later, and that is the delay on the on/off control. Compare the time to connect to port 32042. A slightly weaker signal does not by itself explain a one-second gap when the other lamps on that access point answer in a few tens of milliseconds.

### An ESPHome upload that fails to prepare

`esphome upload` can return `Unknown error from ESP` while receiving the update prepare result. The API port may then stop answering for a short time. Wait until port 6053 accepts a connection and upload the same `firmware.ota.bin` once more. That failure is not by itself a bad image.

## Hardware

| Function | GPIO | Detail |
| --- | --- | --- |
| Warm white | 19 | 2 kHz, duty floor 0.14, ceiling 0.50 |
| Cold white | 21 | 2 kHz, duty floor 0.20, ceiling 0.88 |
| Night | 23 | 20 kHz, duty floor 0.10, ceiling 1.00 |
| Driver enable | 22 | Drops when every channel is 0 |
| Supply sense | 36 | ADC, 12 dB, uncalibrated |

`partitions.csv` is the stock Yeelight table. The two application slots are `miio_fw1` and `miio_fw2`, 1920 KiB each (`1966080` bytes). The image has to fit in one slot. The stock `test` partition is smaller than this firmware. ESPHome prints an overflow for that partition on every build. The two OTA slots still fit the image. Leave the bootloader and the partition table on the chip.

`CONFIG_FREERTOS_UNICORE` is on and `CONFIG_PARTITION_TABLE_MD5` is off. Those two settings are what a miIO 0.0.6 lamp needs in the application image.

## A lamp of your own

Copy `example.yaml` to a name for the fixture. Set `device_id` to that lamp's miIO device id. It becomes the HomeKit serial. Set `node_name` and the HomeKit names. `remote_mac` stays `02:00:00:00:00:01` until you know the dimmer, or call `set_remote` after the lamp is up. The dimmer key is not in the yaml.

To turn the relay on from yaml, add this to the lamp file. A stored receiver on the lamp still replaces it on the next boot.

```yaml
ble_gateway:
  receiver:
    host: 192.168.1.10
    port: 19730
    enabled: true
    key: !secret ble_gateway_key
```

## Configuration

Copy the example and fill it in. `secrets.yaml` is gitignored.

```sh
cp secrets.yaml.example secrets.yaml
```

| Secret | Used for |
| --- | --- |
| `wifi_network`, `wifi_password` | The lamp joins this network. `fast_connect` skips the scan, which is what a hidden SSID needs. |
| `fallback_wifi_password` | The fallback AP. At least 8 characters. |
| `ota_password` | Later ESPHome uploads. |
| `homekit_setup_code` | HomeKit pairing code, `XXX-XX-XXX`. |
| `homekit_setup_id` | Four uppercase letters. Use a different pair per lamp when more than one bridge will be paired. |
| `ble_gateway_key` | 32 hex digits. Read only when a lamp yaml sets `receiver.key`. |

Substitutions in the lamp yaml override `mjxdd01syl.yaml`:

| Key | Meaning |
| --- | --- |
| `node_name` | ESPHome node name and fallback AP prefix |
| `device_id` | miIO device id, used as the HomeKit serial |
| `bridge_name`, `ceiling_name`, `night_name` | HomeKit names |
| `restore_after_power_loss` | `true` or `false` until the switch is stored |
| `default_brightness` | Cold-boot brightness when no level is saved, such as `100%` |
| `default_mireds` | Cold-boot color when no level is saved, 167 to 370 |
| `remote_mac` | Dimmer MAC used until `set_remote` stores one |
| `remote_product_id` | `950` for the YLKG07YL / YLKG08YL |

`components/homekit` and `components/homekit_base` are GPL-3 (license files kept). `tools/append_crc.py` is the Apache-2.0 trailer tool from syssi/esphome-yeelight-ceiling-light.

## Build

From this directory, with ESPHome 2026.10 or newer:

```sh
esphome compile example.yaml
```

The file to upload is:

```text
.esphome/build/<node name>/build/firmware.ota.bin
```

It has to be `1966080` bytes or smaller. `firmware.factory.bin` contains a bootloader and a partition table. Do not upload it.

## Flash a lamp that is already on this firmware

```sh
esphome upload example.yaml --device <lamp-address> \
  --file .esphome/build/<node name>/build/firmware.ota.bin \
  --ota-platform esphome
```

ESPHome reads `ota_password` from `secrets.yaml`. OTA listens on port 3232. Do not pass `--bootloader` or `--partition-table`. The vendor MAC is ignored, so a later boot can take a different DHCP address. If the upload reports `Unknown error from ESP` at the prepare step, wait until port 6053 answers and send the same file once more.

## Flash a lamp that is still on stock firmware

Do these in order. The lamp has to be registered in Xiaomi Home before the update is sent. The update itself goes through that cloud account. This model does not accept `miIO.ota` on local UDP port 54321.

A miIO 0.0.6 updater accepts an application image only when all of the following are true:

- The image was built with `CONFIG_FREERTOS_UNICORE` on and `CONFIG_PARTITION_TABLE_MD5` off. This project already does that.
- A 4-byte Xiaomi trailer is appended with `tools/append_crc.py`.
- The file is served with HTTP/1.1, including Range requests, on port 80. Python's `http.server` speaks HTTP/1.0 and the download does not finish.
- The URL given to the lamp has no port in it: `http://<lan-address-of-the-server>/fw_crc.bin`.
- The bootloader and the partition table on the chip are not written.

miIO 0.0.9 lamps use the same image and the same portless URL. If a 0.0.9 ceiling rejects the trailer, serve `firmware.ota.bin` itself on port 80 with the same URL shape.

### 1. Register the lamp in Xiaomi Home

Add the ceiling in the Xiaomi Home app with the account that will send the update. Wait until that home shows the lamp online. A lamp that is not on the account cannot be selected for the cloud relay.

In the app, or in the cloud device list after the login below, record:

- the device id (`did`), a decimal number
- the region of that account: `cn`, `de`, `us`, `ru`, `tw`, `sg`, `in`, or `i2`
- the lamp's LAN address

Put that device id in the lamp yaml as `device_id` before compiling. It becomes the HomeKit serial. The cloud list must show model `yeelink.light.ceiling22` for that id.

### 2. Build the image and start the file server

Fill in `secrets.yaml` and the lamp yaml, then compile from this directory.

```sh
esphome compile example.yaml
```

The image is `.esphome/build/<node name>/build/firmware.ota.bin`. It has to be `1966080` bytes or smaller. Append the trailer for a miIO 0.0.6 lamp:

```sh
python3 tools/append_crc.py \
  .esphome/build/<node name>/build/firmware.ota.bin \
  firmware.crc.bin
```

Serve that one file on port 80 from a machine the lamp can reach on the LAN. The server answers every path with the same file. Leave it running until the lamp has downloaded the image, then stop it. The image contains the Wi-Fi password and the HomeKit setup code.

```sh
python3 tools/ota_server.py firmware.crc.bin 80
```

`app_url` is `http://<lan-address-of-that-machine>/fw_crc.bin`. There is no port in the URL.

### 3. Log in with a QR code

Password login stops on a captcha. Use the QR login in [Xiaomi Cloud Tokens Extractor](https://github.com/PiotrMachowski/Xiaomi-cloud-tokens-extractor): run it and choose `q`.

The login returns one QR image and a poll timeout of about 300 seconds. Open the image for this attempt, then leave the login process running until it reports that the login succeeded. If that process exits, the poll dies and the code is no longer valid.

Save each attempt as a new PNG and open that file:

```sh
install -m 644 "$downloaded_qr" "xiaomi-qr-$(date +%s).png"
```

A viewer that keeps a file open from the previous path shows the expired code. The extractor also serves the current image at `http://127.0.0.1:31415` while that process is still running. If port 31415 is already taken, it writes a temp file instead. Open the file printed for this attempt.

Scan the code with the Xiaomi Home app on the phone that is logged into the same account, before the timeout. If the poll expires, start the login again, save a new PNG, and scan that file.

Do not write the login URL, the service token, or the account password into a log.

### 4. Confirm the registered lamp, then send the update

With that login, list the devices in the account region. Continue only when the list contains the lamp from step 1: the same `did`, model `yeelink.light.ceiling22`, online, and the LAN address of the ceiling you mean to flash. If it is missing, go back to step 1. Do not send the update to another device id.

The cloud call is encrypted. It goes to `https://<region>.api.io.mi.com/app/home/rpc/<did>`. The China host has no region label: `https://api.io.mi.com/app/home/rpc/<did>`. Use the same logged-in client that listed the device. A plain POST without that encryption is refused.

1. Call `miIO.info`. Keep the stock firmware version and the miIO version it returns.
2. Call `miIO.get_ota_state`. Send the update only when the state is `idle`.
3. Call `miIO.ota` with one parameter, `app_url` set to the portless URL from step 2. The result is `ok` when the relay accepted it.
4. Call `miIO.get_ota_state` about every 5 seconds. Stop when the state is `installed` or `failed`.

The HTTP log from `ota_server.py` must show one complete GET of the whole file from the lamp's address. A complete download, followed by the ESPHome API on port 6053 and HomeKit on port 32042, means the image is on the lamp even when the cloud state comes back empty after the reboot. Do not send a second `miIO.ota` after that complete download.

Stop `ota_server.py` once the download has finished.

The lamp reboots as `node_name`. Stock HomeKit does not come with it. Pair the new bridge on port 32042 with `homekit_setup_code`. If discovery skips the accessory because it still looks paired, press "Reset HomeKit pairings" once, then pair.

After that, further updates use the ESPHome upload in the previous section.
