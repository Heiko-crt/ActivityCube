directory: curl.exe http://ESP32-IP/files/
file upload: curl.exe --upload-file test.txt "http://192.168.43.72/files/test.txt" --> Hat funktioniert
Read: http://192.168.43.72/files/test.txt
Read mit Curl: curl.exe "http://192.168.43.72/files/test.txt"

## Wi-Fi credentials

Copy `src/wifi_credentials.h.example` to `src/wifi_credentials.h` and set your
local Wi-Fi SSID and password there. The credentials header is ignored by Git;
do not commit it. Without this local file, Wi-Fi startup reports that the SSID
is not configured.
