# Pinned ESP-IDF toolchain for the Rainlog Wireless Bridge.
# All builds run in this image (see build.sh); no host toolchain needed.
# v6.0.1 is the latest stable ESP-IDF and supports the ESP32-C6 target.
FROM espressif/idf:v6.0.1

WORKDIR /project
