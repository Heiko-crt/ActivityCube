curl.exe http://192.168.1.42/files/
curl.exe -X PUT --data-binary "@sensor.csv" http://192.168.1.42/files/sensor.csv

directory:
curl.exe http://<ESP32-IP>/files
