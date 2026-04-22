# XIAO-Esp-32-s3-sense-webpage-stream
ESP32 Camera Live Streaming Project
Project Overview
This project is based on the XIAO ESP32-S3 Sense and is designed to provide a live camera streaming system over Wi-Fi. The board is configured to act as a hotspot (Access Point), allowing a user to connect directly to it and open a webpage where the live camera feed can be viewed in real time.

To extend the functionality of the system, an additional ESP32 board with a display can also be connected. This allows the same camera stream to be shown not only on the webpage, but also on a separate display device. As a result, the project supports dual-output streaming, where the video feed can be monitored through both a browser and an external ESP32-based display.

The project uses the Adafruit library for display and hardware-related functionality. Overall, this system demonstrates how embedded devices can be used for wireless video streaming, web-based monitoring, and display integration in a compact and efficient way.

Features
Live camera streaming using XIAO ESP32-S3 Sense
Wi-Fi hotspot mode for direct device connection
Real-time camera feed displayed on a webpage
Support for an additional ESP32 with a display
Simultaneous streaming to both webpage and external display
Embedded implementation using the Adafruit library
Compact and portable wireless monitoring solution
Hardware Used
Seeed Studio XIAO ESP32-S3 Sense
Camera module integrated with the XIAO ESP32-S3 Sense
Additional ESP32 board with display
USB cable for programming and power
Computer or mobile device to access the webpage
Power source for the ESP32 modules
Software Requirements
Arduino IDE or compatible ESP32 development environment
ESP32 board package installed
Required Adafruit library
Wi-Fi-enabled browser for viewing the live stream
Libraries Used
Adafruit library for display handling and hardware support
If multiple Adafruit libraries were used in your code, you can replace this section with the exact library names.

Working Principle
The XIAO ESP32-S3 Sense captures live images using its onboard camera. It is configured as a Wi-Fi hotspot, which allows nearby devices such as laptops or smartphones to connect directly to it without needing an external router.

Once connected, the user can open the hosted webpage in a browser and view the live camera stream. In addition, the system is designed so that another ESP32 with a display can receive and show the same stream. This creates a setup where one device acts as the camera streaming source, while another device works as a remote viewing display.

This architecture is useful for applications that require portable monitoring, local wireless streaming, and multi-device viewing.

Applications
Wireless surveillance prototypes
Smart monitoring systems
IoT camera display systems
Portable live-view camera projects
Embedded web server camera applications
Real-time display sharing between ESP32 devices
Advantages
Does not require an external Wi-Fi router
Easy to access through a browser
Supports multiple viewing methods
Compact and low-power embedded solution
Useful for real-time monitoring and demonstration projects
Limitations
Streaming quality depends on ESP32 processing capability and network stability
Range is limited by hotspot signal strength
Frame rate may vary depending on resolution and connected devices
External display performance depends on the specifications of the second ESP32 display module
