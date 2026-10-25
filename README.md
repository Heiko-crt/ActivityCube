curl.exe http://ESP32-IP/files/
curl.exe -X PUT --data-binary "@sensor.csv" http://192.168.1.42/files/sensor.csv
Read: GET http://<ESP32-IP>/files/<filename>
Write or replace: PUT http://<ESP32-IP>/files/<filename>

directory:
curl.exe http://ESP32-IP/files
