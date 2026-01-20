# Mini Web Radio 3 - 5Ghz Backpack
This project upgrades [MWR v3](https://github.com/AllanGallop/mini-web-radio-3) boards to use 5Ghz wifi as an interim measure until Expressif release the dual radio - dual cored "C22".

## Hardware
A flexible PCB is used to attach an ESP32-C5 to the existing v3 board. Be sure to connect the *3v3* to the `ASO` LDO regulator and *GND* to any via in the ground pour. 


[<img src=".github/images/board_render.png" height="400" width="180"/>](.github/images/board_render.png)

## Firmware

Both boards require new firmware to be compiled and uploaded. _Currently this release does not include the previous configuration provisions._

### Mini-Web-Radio v3 (S3)

1. Open the S3 directory in PlatformIO
2. Build
3. Upload

### Backpack (C5)

1. Open the C5 directory in PlatformIO
2. Edit main.cpp
    - set `WIFI_SSID`, `WIFI_PASS` to match your wi-fi network.
    - set `HOST`, `PORT` & `PATH` to match your audio stream.
3. Build
4. Upload 