# Pinned ESP-IDF toolchain for the Rainlog Wireless Bridge.
# All builds run in this image (see build.sh); no host toolchain needed.
# v6.0.1 supports both ESP32-C6 and ESP32 targets.
FROM espressif/idf:v6.0.1

WORKDIR /project
