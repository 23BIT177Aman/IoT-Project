# IoT-Project
# 💊 Smart Medicine Storage and Monitoring System

## Project Overview
The **Smart Medicine Storage and Monitoring System** is an IoT-based project designed to monitor environmental conditions inside medicine refrigerators. It uses ESP32, LoRa communication and cloud integration to provide real-time temperature and humidity monitoring, helping maintain suitable storage conditions for temperature-sensitive medicines.

The system collects sensor readings every 5 seconds and transmits them wirelessly to a central gateway using a 433 MHz LoRa connection. The gateway displays the readings on a local web dashboard and uploads the data to Firebase for remote monitoring.

## Key Features
- Real-time temperature monitoring using DS18B20.
- Humidity monitoring using DHT22.
- Wireless data transmission using 433 MHz LoRa.
- Local Wi-Fi dashboard for live sensor readings.
- Firebase Realtime Database integration.
- Threshold-based temperature and humidity alerts.
- Medicine inventory and reminder management.
- Historical data monitoring and device status detection.

## Technologies & Components
- **Microcontroller:** ESP32 DevKit V1
- **Communication:** SX1278 LoRa Module
- **Sensors:** DS18B20 and DHT22
- **Cloud Database:** Firebase Realtime Database
- **Programming:** Arduino IDE (Embedded C++)
- **Web Technologies:** HTML, CSS and JavaScript

## System Architecture
The project consists of two ESP32-based nodes:

1. **Fridge Node:** Collects temperature and humidity readings and transmits them through LoRa.
2. **Hub Gateway:** Receives sensor data, manages the local dashboard and uploads readings to Firebase.

The cloud dashboard provides access to live readings, historical data, alerts, medicine records and reminders.

## Project Team
Developed as part of the **Internet of Things Lab (20IC401P)** at **Pandit Deendayal Energy University (PDEU)**, School of Technology, Department of Information & Communication Technology.

**Academic Year:** 2025–2026
