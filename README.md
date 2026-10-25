directory: curl.exe http://ESP32-IP/files/
file upload: curl.exe --upload-file test.txt "http://192.168.43.72/files/test.txt" --> Hat funktioniert
Read: http://192.168.43.72/files/test.txt
Read mit Curl: curl.exe "http://192.168.43.72/files/test.txt"

