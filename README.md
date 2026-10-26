directory: curl.exe http://ESP32-IP/files/
file upload: curl.exe --upload-file test.txt "http://192.168.43.72/files/test.txt" --> Hat funktioniert
Read: http://192.168.43.72/files/test.txt
Read mit Curl: curl.exe "http://192.168.43.72/files/test.txt"

## MPU6050 orientation sensor

Connect the MPU6050 module to the ESP32 DevKit as follows:

| MPU6050 | ESP32 DevKit |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

For the ESP32-C3 target, the driver uses GPIO 4 (SDA) and GPIO 5 (SCL).
Use 3.3 V-compatible I2C pull-ups; many modules already include them. The
driver checks both common I2C addresses (0x68 and 0x69) and accepts WHO_AM_I
values 0x68 (MPU-6050) and 0x70 (MPU-6500).

`mpu6050_read_orientation()` returns roll, pitch, and yaw in degrees.
Roll and pitch use accelerometer/gyroscope complementary filtering. Yaw is
integrated from the gyroscope only, so it is relative and will drift over time
because the MPU6050 has no magnetometer. When detected, the application outputs
all three angles to UART every 100 ms and serves them as JSON at `/orientation`.
The home page displays the angles and refreshes them once per second.
The UART log also records each MPU6050 I2C transaction: 7-bit device address,
register/data bytes sent, received bytes, and the ESP-IDF result. Successful
transaction results mean the driver observed ACKs; failures report the driver
error (for example, a NACK or timeout). Because sensor samples are read every
100 ms, these diagnostics produce frequent log output while the sensor is active.

## Wi-Fi credentials

Copy `src/wifi_credentials.h.example` to `src/wifi_credentials.h` and set your
local Wi-Fi SSID and password there. The credentials header is ignored by Git;
do not commit it. Without this local file, Wi-Fi startup reports that the SSID
is not configured.
