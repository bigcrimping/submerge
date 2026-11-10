# submerge
IN-9 based wave meter running on ESP8266.

![submerge](https://github.com/bigcrimping/submerge/assets/74270551/9abd5a54-fe4b-47b1-9596-4b3b62df4726)

The ESP8266 connects to the API from Coastalmonitoring.org https://coastalmonitoring.org/ and pulls the latest wave height and wave period from the chosen buoy and converts it to a PWM signal to drive a IN-9 device


https://github.com/bigcrimping/submerge/assets/74270551/b3d905b5-1866-4489-b5fa-f626d71e5c9f



## Project Code

The code is here: https://github.com/bigcrimping/submerge/tree/main/code/buoy_code

You will need to install the following:

https://github.com/esp8266/Arduino/tree/master
https://arduinojson.org/ (version 7)

### WiFi and API details

The sketch needs your WiFi name and password, plus the API key and Referer from https://coastalmonitoring.org/ccoresources/api/. There are two ways to supply them:

1. **Over serial (no file editing).** Flash the sketch, open the serial monitor at 115200 baud and answer the prompts. The answers are saved to flash and survive power cycles. To change them later, press Enter in the serial monitor within 5 seconds of boot. Leave the WiFi password empty for an open network.
2. **In a header.** Copy `buoy_secrets.example.h` to `buoy_secrets.h` and fill it in. `buoy_secrets.h` is ignored by git so your details are not committed by accident.

Whichever was changed most recently wins: values entered over serial override the header, until you flash a build with a different `buoy_secrets.h`.

### How it works

The routine "grab_data" requests the current wave height (hmax, falling back to hs when the buoy does not report hmax) and peak period (tp) from the API. The API requires setting two custom HTTP headers. Data is fetched every 30 minutes, matching how often the buoys report; if the request itself fails it is retried after 1 minute, backing off to 30 minutes.

Each record's timestamp is checked against the server's clock, because a buoy that has stopped reporting keeps returning its last record. Records more than 3 hours old are ignored.

The display routine converts the wave period into a delay between each of the 85 samples in "wave_array[]" and scales it via the wave height.

The output full scale (PWM 255) is scaled to a max wave height of 6m which is a compromise between accuracy in extremely  high wave conditions and not looking boring at normal (0.8-1.2m) heights

If the latest record is too old, or no good reading has arrived for 3 hours, the wave stops and the tube holds a steady low glow, so old data is not mistaken for live conditions.

The API requests the following attribution:

"Real time data displayed on this page are from the National Network of Regional Coastal Monitoring Programmes, made freely available under the terms of the Open Government Licence. Please note that data are not quality-controlled in real-time."

## Wiring

I'm usually quite happy to share wiring diagrams, but this project contains 150V power supply which at best is extremely unpleasant to touch and at worst could cause serious injury.

If you want to copy this project, get someone who understands the risks to look over your circuit before powering it up.

For the PWM current drive I stole the design from https://www.limpkin.fr/index.php?post/2021/05/24/The-IN-9-Nixie-Clock

The power supply is a generic "DC5V-12V to 170V 150V-220V Glow Tube High Voltage Boost DC Power Module F/ Nixie" from the usual sort of vendors
You may need to burn your IN-9 tube in at higher current (20mA) for several hours to get the tube to light to the top.


## Mechanical Files

![image](https://github.com/bigcrimping/submerge/assets/74270551/2a624e83-85c1-410f-90c2-38e8f427b256)


The design comprises (from top to bottom)  
Front
Spiral
base

When printing the spiral you will want to block supports inside the tube or you won't be able to get the wires to the IN-9 in

To attach the "front" to the "spiral" requires a 4.4mm diameter M2.5 insert, the base requires five 3.5mm diameter M2.5 inserts

![image](https://github.com/bigcrimping/submerge/assets/74270551/bdf7de5c-089e-4570-b545-d2a87eb92589)


3MF files for printing available here: https://github.com/bigcrimping/submerge/tree/main/mech



