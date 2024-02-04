
#include <Arduino.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include "wave_array.h"
#include "buoy_secrets.h"

ESP8266WiFiMulti WiFiMulti;
JsonDocument doc;

struct waveFloats_t {
 float hmax = 6.0;
 float tp = 10;
} wave_data;

const int nixiePin = 16;  //Output pin the IN-9 is controlled from

int loops = 100;

void setup() {
  pinMode(nixiePin,OUTPUT);
  analogWriteFreq(200);     //PWM frequency can be adapted to your tube
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(SECRET_SSID, SECRET_PASS);
  delay(2000);
  
}

waveFloats_t grab_data (){
  waveFloats_t data_out;


    if ((WiFiMulti.run() == WL_CONNECTED)) {

    std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
    client->setInsecure();
    HTTPClient https;

    Serial.print("[HTTPS] begin...\n");
    if (https.begin(*client, "https://coastalmonitoring.org/observations/waves/latest.geojson?sensor=113&duration=0")) {  // the "sensor=13" defines the sensor, in this case St Mary's Sound 

      
      https.addHeader("Content-Type", "application/json"); 
      https.addHeader("X-API-Key", SECRET_KEY);
      https.addHeader("Referer", SECRET_REF);

      Serial.print("[HTTPS] GET...\n");
      int httpCode = https.GET();
      Serial.printf("Response code: %u\n",httpCode);
      Serial.printf("Content length: %u\n",https.getSize());
    
      if (httpCode > 0) {
        Serial.printf("[HTTPS] GET... code: %d\n", httpCode);

        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
          String payload = https.getString();
          Serial.println(payload);

          DeserializationError error = deserializeJson(doc, payload);

          if (error) {
            Serial.print("deserializeJson() failed: ");
            Serial.println(error.c_str());
            data_out.hmax = 1.0;
            data_out.tp = 10;

            
          }

          const char* type = doc["type"];

          JsonObject features_0 = doc["features"][0];
          const char* features_0_type = features_0["type"]; 

          JsonObject features_0_properties = features_0["properties"];


          float hmax = features_0_properties["hmax"]; // Value for waveheight
          float tp = features_0_properties["tp"]; // Value for time period


          data_out.hmax = hmax;
          data_out.tp = tp;

        }
      } else {
        
        Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
      }

      https.end();
    } else {
      Serial.printf("[HTTPS] Unable to connect\n");
    }

  }



  return data_out;
}

void loop() {
  
  waveFloats_t output_data= grab_data();
  float loop_delay = (output_data.tp / 84)*1000; //Convert wave period into 84 chunks of milliseconds

  while(1){
    if (loops = 0){
      waveFloats_t output_data= grab_data();
      
      loops = 100;
    }
    else{
      loops--;
    }
  

    Serial.print("Waveweight: ");
    Serial.println(float(output_data.hmax));
    Serial.print("Period: ");
    Serial.println(float(output_data.tp));
    
    float loop_delay = (output_data.tp / 84)*1000; //Convert wave period into 84 chunks of milliseconds

    
    for(int i = 0; i< 84; i++){
      float display_value = ((wave_array[i]*(output_data.hmax/2))+(output_data.hmax/2)); 
      //Serial.println(String(display_value));
      float intermediate = display_value * 43; //Max output is 255, scale the input max to be 6m/255 = 43 scaling factor
      analogWrite(nixiePin, int(intermediate));
      delay(loop_delay);
    }
  }
}
