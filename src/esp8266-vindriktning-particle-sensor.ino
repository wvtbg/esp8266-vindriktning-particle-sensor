#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <coap-simple.h>

#include "Config.h"
#include "SerialCom.h"
#include "Types.h"

particleSensorState_t state;

uint8_t mqttRetryCounter = 0;

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient;

WiFiManagerParameter custom_mqtt_server("server", "MQTT server", Config::mqtt_server, sizeof(Config::mqtt_server));
WiFiManagerParameter custom_mqtt_user("user", "MQTT username", Config::username, sizeof(Config::username));
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", Config::password, sizeof(Config::password));
WiFiManagerParameter custom_mqtt_topic("topic", "MQTT Topic", Config::mqtt_topic, sizeof(Config::mqtt_topic));
WiFiManagerParameter custom_coap_server("coapserver", "Coap server", Config::coap_server, sizeof(Config::coap_server));

uint32_t lastMqttConnectionAttempt = 0;
const uint16_t mqttConnectionInterval = 60000; // 1 minute = 60 seconds = 60_000 milliseconds

uint32_t statusPublishPreviousMillis = 0;
const uint32_t statusPublishInterval = 300000; // 5 minutes = 300 seconds = 300_000 milliseconds

char identifier[24];
/**#define FIRMWARE_PREFIX "esp8266-vindriktning-particle-sensor"*/
#define AVAILABILITY_ONLINE "{ \"state\" : \"online\" }"
#define AVAILABILITY_OFFLINE "{ \"state\" : \"offline\" }"
char MQTT_TOPIC_STATE[128];

// UDP and CoAP class
WiFiUDP udp;
Coap coap(udp);

IPAddress coapAddress;
bool coapSet = false;
boolean mqttSet = false;

// CoAP client response callback
void callback_response(CoapPacket &packet, IPAddress ip, int port);

// CoAP server endpoint url callback
void callback_light(CoapPacket &packet, IPAddress ip, int port);


bool shouldSaveConfig = false;

void saveConfigCallback() {
    shouldSaveConfig = true;
}

void setup() {
    Serial.begin(115200);
    SerialCom::setup();

    Serial.println("\n");
    Serial.println("Hello from esp8266-vindriktning-particle-sensor");
    Serial.printf("Core Version: %s\n", ESP.getCoreVersion().c_str());
    Serial.printf("Boot Version: %u\n", ESP.getBootVersion());
    Serial.printf("Boot Mode: %u\n", ESP.getBootMode());
    Serial.printf("CPU Frequency: %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Reset reason: %s\n", ESP.getResetReason().c_str());

    delay(3000);

    snprintf(identifier, sizeof(identifier), "VINDRIKTNING-%X", ESP.getChipId());

    WiFi.hostname(identifier);

    loadConfig();
    
    setupWifi();
    setupOTA();
    
    mqttSet = strlen(Config::mqtt_server) > 0;

    coapSet = WiFi.hostByName(Config::coap_server, coapAddress);

    snprintf(MQTT_TOPIC_STATE, 127, Config::mqtt_topic, identifier);
    Serial.printf("MQTT server: %s\n", Config::mqtt_server);
    Serial.printf("MQTT Topic State: %s\n", MQTT_TOPIC_STATE);
    
    mqttClient.setServer(Config::mqtt_server, 1883);
    mqttClient.setKeepAlive(10);
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(mqttCallback);

    Serial.printf("COAP server: %s\n", Config::coap_server);

    Serial.printf("Hostname: %s\n", identifier);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    Serial.println("-- Current GPIO Configuration --");
    Serial.printf("PIN_UART_RX: %d\n", SerialCom::PIN_UART_RX);

    mqttReconnect();
}

void setupOTA() {
    ArduinoOTA.onStart([]() { Serial.println("Start"); });
    ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            Serial.println("End Failed");
        }
    });

    ArduinoOTA.setHostname(identifier);

    // This is less of a security measure and more a accidential flash prevention
    ArduinoOTA.setPassword(identifier);
    ArduinoOTA.begin();
}

void loop() {
    ArduinoOTA.handle();
    SerialCom::handleUart(state);
    mqttClient.loop();
    wifiManager.process();
    if(shouldSaveConfig){
      //save if needed
      Serial.println("Get Values from web and save\n");
      strcpy(Config::mqtt_server, custom_mqtt_server.getValue());
      strcpy(Config::username, custom_mqtt_user.getValue());
      strcpy(Config::password, custom_mqtt_pass.getValue());
      strcpy(Config::mqtt_topic, custom_mqtt_topic.getValue());
      strcpy(Config::coap_server, custom_coap_server.getValue());
      
      Config::save();
      snprintf(MQTT_TOPIC_STATE, 127, Config::mqtt_topic, identifier);
      Serial.printf("MQTT Topic State: %s\n", MQTT_TOPIC_STATE);
      //reset save flag
      shouldSaveConfig = false;
      loadConfig();
    }

    const uint32_t currentMillis = millis();
    // reset previous for time flip over after 50 days
    if (currentMillis < statusPublishPreviousMillis){
        printf("Clock flip to zero reset previous millis");
        statusPublishPreviousMillis = currentMillis;
    }
    if (currentMillis - statusPublishPreviousMillis >= statusPublishInterval) {
        statusPublishPreviousMillis = currentMillis;

        if (state.valid) {
            printf("Publish state\n");
            publishState();
        }
    }

    if (mqttSet && !mqttClient.connected() && currentMillis - lastMqttConnectionAttempt >= mqttConnectionInterval) {
        lastMqttConnectionAttempt = currentMillis;
        printf("Reconnect mqtt\n");
        mqttReconnect();
    }
    
}

void setupWifi() {
    wifiManager.setDebugOutput(false);    

    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);
    wifiManager.addParameter(&custom_mqtt_topic);
    wifiManager.addParameter(&custom_coap_server);

    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setPreSaveConfigCallback(saveConfigCallback);


    WiFi.hostname(identifier);
    wifiManager.autoConnect(identifier);
    mqttClient.setClient(wifiClient);

    strcpy(Config::mqtt_server, custom_mqtt_server.getValue());
    strcpy(Config::username, custom_mqtt_user.getValue());
    strcpy(Config::password, custom_mqtt_pass.getValue());
    strcpy(Config::mqtt_topic, custom_mqtt_topic.getValue());
    strcpy(Config::coap_server, custom_coap_server.getValue());

    if (shouldSaveConfig) {
        Config::save();
        //reset save flag
        shouldSaveConfig = false;
    } else {
        // For some reason, the read values get overwritten in this function
        // To combat this, we just reload the config
        // This is most likely a logic error which could be fixed otherwise
        loadConfig();
    }
    
    wifiManager.startWebPortal();
}

void loadConfig(){
    Config::load();

    if (strlen(Config::mqtt_server) > 0 ){
        mqttSet = true;
        custom_mqtt_server.setValue(Config::mqtt_server, strlen(Config::mqtt_server));
    }else
        mqttSet = false;
    if (strlen(Config::username) > 0 ) custom_mqtt_user.setValue(Config::username, strlen(Config::username));
    if (strlen(Config::password) > 0 ) custom_mqtt_pass.setValue(Config::password, strlen(Config::password));
    if (strlen(Config::mqtt_topic) > 0 ) custom_mqtt_topic.setValue(Config::mqtt_topic, strlen(Config::mqtt_topic));
    if (strlen(Config::coap_server) > 0 ){
        coapSet = WiFi.hostByName(Config::coap_server, coapAddress);
        custom_coap_server.setValue(Config::coap_server, strlen(Config::coap_server));
    } else
        coapSet = false;

}

void resetWifiSettingsAndReboot() {
    wifiManager.resetSettings();
    delay(3000);
    ESP.restart();
}

void mqttReconnect() {
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        if (mqttClient.connect(identifier, Config::username, Config::password, MQTT_TOPIC_STATE, 1, true, AVAILABILITY_OFFLINE)) {
            mqttClient.publish(MQTT_TOPIC_STATE, AVAILABILITY_ONLINE, true);
            //publishAutoConfig();

            // Make sure to subscribe after polling the status so that we never execute commands with the default data
            //mqttClient.subscribe(MQTT_TOPIC_COMMAND);
            break;
        }
        delay(5000);
    }
}

bool isMqttConnected() {
    return mqttClient.connected();
}

void publishState() {
    DynamicJsonDocument wifiJson(192);
    DynamicJsonDocument stateJson(604);
    char payload[256];

    wifiJson["ssid"] = WiFi.SSID();
    wifiJson["ip"] = WiFi.localIP().toString();
    wifiJson["rssi"] = WiFi.RSSI();

    stateJson["pm25"] = state.avgPM25;

    stateJson["wifi"] = wifiJson.as<JsonObject>();

    serializeJson(stateJson, payload);
    if (mqttSet){
        printf("Send state to MQTT\n");
        mqttClient.publish(&MQTT_TOPIC_STATE[0], &payload[0], true);
    }
    if (coapSet){
        printf("Send state to COAP\n");
        char coapPayload[16];
        // serial (6 last mac) version (1) ppm 2.5 (2D)
        sprintf(coapPayload, "%X01%04x", ESP.getChipId(), state.avgPM25) ;

        coap.put(coapAddress, 5683, "VINDRIKTNING", coapPayload);
    }
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) { }

// CoAP client response callback
void callback_response(CoapPacket &packet, IPAddress ip, int port) {
  Serial.println("[Coap Response got]");

  char p[packet.payloadlen + 1];
  memcpy(p, packet.payload, packet.payloadlen);
  p[packet.payloadlen] = NULL;

  Serial.println(p);
}

void callback_response(CoapPacket &packet, IPAddress ip, int port);