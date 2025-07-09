# Advanced ISR-Driven PV Power Diverter with ESPHome Integration

This project is a significantly enhanced and modernized version of the classic Mk2 PV Router code, originally developed by Robin Emley. While the core principle of diverting surplus solar PV energy to a resistive load (like a hot water heater) remains, this version introduces a more robust, configurable, and connectable architecture suitable for modern smart home integration.

The system is architected as a two-part solution:

1. An **Arduino Uno** acts as a high-speed, dedicated measurement co-processor. Its sole job is to perform time-critical power calculations and report the status.

2. An **ESP32/ESP8266 running ESPHome** acts as the "brain". It receives data from the Arduino, contains all the control logic, and integrates everything seamlessly into Home Assistant.

## Key Features & Improvements

This version builds upon the original code with several major enhancements:

* **ISR-Driven Architecture:** All time-critical processing—ADC sampling, power calculations, and load-switching decisions—has been moved directly into a Timer-based Interrupt Service Routine (ISR) on the Arduino. This makes the system highly reliable and predictable.

* **Persistent Settings in EEPROM:** Key calibration and operational parameters are no longer hard-coded. They are stored in the Arduino's EEPROM, meaning they persist through power cycles.

* **On-the-Fly Serial Configuration:** Connect directly to the Arduino via the Serial Monitor to view, change, and save all operational parameters without needing to recompile and re-upload the sketch.

* **Full ESPHome Integration:** The provided ESPHome configuration creates a complete set of controls and sensors in Home Assistant for full monitoring and automation.

* **Automatic "Tank Full" Detection:** The system intelligently deduces when the hot water tank is full by monitoring power flows, eliminating the need for a dedicated temperature sensor. It automatically pauses diversion when the tank is hot and resumes when it detects the tank can accept more energy.

* **Fail-Safe Watchdog Timer:** An onboard watchdog timer ensures that if the Arduino's software ever hangs, it will automatically reboot, ensuring maximum uptime and reliability.

## Working Principle

The Arduino continuously measures the real power at the grid connection point. It uses an ISR running thousands of times per second to achieve high accuracy.

The system uses an "energy bucket" concept to smooth out its response and prevent rapid, unnecessary switching of the load. At the end of each mains cycle, the net energy (imported or exported) is added to or subtracted from this bucket. The diverter only turns the load ON when the bucket is full (indicating consistent surplus) and OFF when it is empty (indicating consistent import).

The size of this bucket is controlled by the `WRJ` (Working Range in Joules) setting. This allows you to fine-tune the diverter's sensitivity and response time. A larger `WRJ` value creates a bigger "energy bucket," which means the diverter must see a sustained surplus for a longer period before it activates.

This is particularly useful in systems with a home storage battery. As described by Robin Emley, setting a large `WRJ` (e.g., 28800, which corresponds to 8Wh) creates an asymmetrical response. It effectively delays the start-up of the diverter, giving a faster-acting battery control system plenty of time to stabilize and claim the initial surplus. However, when the export condition ends, the diverter will still deactivate quickly, preventing unnecessary power draw from the battery.

The control method used is a form of **Burst Fire** control. The decision to turn the load ON or OFF is made once per mains cycle, and the load is then switched for many full cycles at a time. This method is far superior to Phase Control for this application as it generates significantly less electrical noise (EMI) and is much kinder to the grid and other appliances in your home.

The core logic resides on the ESPHome device. The Arduino sends a simple, comma-separated status update over the serial connection every few seconds.

**Example Serial String:** `1,0,0,1,0,1234`

The ESPHome device parses this string and updates a series of sensors and switches in Home Assistant. When you toggle a switch in Home Assistant (e.g., "PV Diverter Boost"), ESPHome sends a command back to the Arduino (e.g., `FULL_LOAD_ON`), which then adjusts its behavior accordingly.

This separation of concerns makes the system both powerful and easy to manage, as all complex logic can be updated wirelessly via ESPHome.

## Arduino Serial Command Interface

You can connect to the Arduino using the Arduino IDE's Serial Monitor at **9600 baud** with "Newline" line ending.

| Command | Description | 
 | ----- | ----- | 
| `HELP` | Displays the list of available commands. | 
| `STATUS` | Shows the current settings loaded in memory. | 
| `SAVE` | Saves the current settings to permanent EEPROM storage. | 
| `LOAD` | Discards any unsaved changes and reloads settings from EEPROM. | 
| `RESET` | Resets all settings to their factory defaults and saves. | 
| `ENABLE` / `DISABLE` | Controls overall diverter operation. | 
| `FULL_LOAD_ON` / `OFF` | Controls the "boost" mode to force the load on. | 
| `SET <param> <val>` | Changes a setting in memory (use `SAVE` to make permanent). | 

### EEPROM Settable Parameters

These parameters can be changed on-the-fly using the `SET` command.

| Param | Default | Description | Example Command | 
 | ----- | ----- | ----- | ----- | 
| `PCG` | `0.0459` | **PowerCal Grid:** The calibration factor for your grid CT. | `SET PCG 0.045` | 
| `PCD` | `0.0435` | **PowerCal Diverted:** The calibration factor for the diverted load CT. | `SET PCD 0.043` | 
| `WRJ` | `7200` | **Working Range Joules:** The size of the "energy bucket". A larger value (e.g., 28800) delays startup to prioritize a battery. | `SET WRJ 28800` | 
| `REW` | `0` | **Required Export Watts:** Target export level. Use `-100` to prioritize a battery, or `50` to prioritize the diverter. | `SET REW -100` | 
| `ACL` | `5` | **Anti-Creep Limit:** Threshold in Joules to prevent noise from accumulating as energy. | `SET ACL 5` | 
| `DEBUG` | `0` | **Debug Mode:** `1` for Human-Readable logs, `0` for IoT format. | `SET DEBUG 1` | 

## ESPHome Integration

The provided ESPHome configuration creates the following entities in Home Assistant:

### Switches

* **PV Diverter Control:** Master switch to enable or disable the automatic diversion logic.

* **PV Diverter Boost:** A switch to force the hot water load ON, regardless of surplus.

### Binary Sensors

* **Diverter Status:** Shows whether the diverter is enabled or disabled.

* **Diverter Full Load:** Shows if the Boost mode is active.

* **Tank Is Full:** `ON` when the system has automatically detected the hot water tank is full.

* **Grid Exporting:** `ON` when there is a significant amount of surplus power being exported to the grid.

* **Diverter Load On:** `ON` when the diverter is actively sending power to the hot water element.

### Sensor

* **Diverted Energy:** A running total of the energy (in Watt-hours) that has been diverted to the hot water load for the current day. Resets automatically.

## Licensing

This project is open source and licensed under the MIT License.

Copyright (c) 2025

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Credits

* This project is based on the foundational work of **Robin Emley** and his original Mk2 PV Router.

* The code has been significantly refactored, modernized, and improved with the assistance of **Google's Gemini 2.5 Pro**.
