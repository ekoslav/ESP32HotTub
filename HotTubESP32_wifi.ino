// Hot Tub Controller - ESP32 WROOM32
// Balboa EL2000 replacement logic
// CONTROL PIN DEFINITIONS
// Speaker+ - D2
// Speaker- - GND
// PUMP1A Low Speed - D13 ORANGE
// PUMP1B HI Speed - D12 RED
// RELAY_PUMP2 - D14 GREEN
// RELAY_PUMP3 - D27 PURPLE
// RELAY_LIGHTS - D25 BROWN
// RELAY_HEATER_RED- D32 BLUE
// RELAY_HEATER_BLACK- D33 YELLOW
// TEMP_SENSOR_1 - D36 (VP)
// TEMP_SENSOR_2 - D39 (VN)

#define RELAY_PUMP1LO 13
#define RELAY_PUMP1HI 12
#define RELAY_PUMP2 14
#define RELAY_PUMP3 27
#define RELAY_LIGHT 25
#define RELAY_HEATER_RED 32
#define RELAY_HEATER_BLACK 33
#define SPEAKER_PIN 2
#define TEMP_SENSOR_1 36
#define TEMP_SENSOR_2 39

#include <WiFi.h>
#include <WebServer.h>
#include <Ticker.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <HTTPUpdate.h>

//String version = "esp32hottub_20050427001";
String version = String(__DATE__) + " " + String(__TIME__);

WebServer server(80);
DNSServer dnsServer;
Ticker heaterTicker;
Ticker overheatLightTicker;
bool blinkingLights = false;
bool heaterDisabled = false;

const char* firmwareURL = "https://github.com/ekoslav/ESP32HotTub/releases/latest/download/firmware.bin";
const char* latestVersionURL = "https://github.com/ekoslav/ESP32HotTub/releases/latest/download/version.txt";
//const char* firmwareURL = "https://raw.githubusercontent.com/ekoslav/ESP32HotTub/main/firmware.bin";
//const char* latestVersionURL = "https://raw.githubusercontent.com/ekoslav/ESP32HotTub/main/version.txt";


String ssid = "defaultSSID";
String password = "defaultPASS";
float currentTemp = 0;
float sensor1Offset = 0;
float sensor2Offset = 0;
float sensor2Temp = 0;
float targetTemp = 39.0;
bool lightRequested = false; 
bool heaterOn = false;
bool overheat = false;
unsigned long heaterStartTime = 0;
unsigned long heaterStopTime = 0;
bool heaterRequested = false;
bool pump1Running = false;
bool pump1WasRunning = false;
unsigned long pump1StartTime = 0;
unsigned long heaterCooldownStart = 0;
float temp1Sum = 0;
float temp2Sum = 0;
unsigned int tempSamples = 0;
unsigned long lastAverageTime = 0;
float averagedTemp1 = 0;
float averagedTemp2 = 0;

String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void beep() {
  tone(SPEAKER_PIN, 1000, 100);
  delay(150);
  noTone(SPEAKER_PIN);
}

void beepDouble() {
  beep(); delay(150); beep();
}

void beepTripleSlow() {
  tone(SPEAKER_PIN, 800, 200); delay(400); 
  tone(SPEAKER_PIN, 800, 200); delay(400);
  tone(SPEAKER_PIN, 800, 200); delay(400);
  noTone(SPEAKER_PIN);
}
void beepOverheatWarning() {
  tone(SPEAKER_PIN, 2000, 100); delay(150);
  noTone(SPEAKER_PIN); delay(150);
}

void startupSound() {
  tone(SPEAKER_PIN, 880, 150); delay(200);
  tone(SPEAKER_PIN, 1320, 150); delay(200);
  tone(SPEAKER_PIN, 1760, 150); delay(200);
  noTone(SPEAKER_PIN);
}

void setupRelays() {
  pinMode(RELAY_PUMP1LO, OUTPUT);
  pinMode(RELAY_PUMP1HI, OUTPUT);
  pinMode(RELAY_PUMP2, OUTPUT);
  pinMode(RELAY_PUMP3, OUTPUT);
  pinMode(RELAY_LIGHT, OUTPUT);
  pinMode(RELAY_HEATER_RED, OUTPUT);
  pinMode(RELAY_HEATER_BLACK, OUTPUT);

  digitalWrite(RELAY_PUMP1LO, LOW);
  digitalWrite(RELAY_PUMP1HI, LOW);
  digitalWrite(RELAY_PUMP2, LOW);
  digitalWrite(RELAY_PUMP3, LOW);
  digitalWrite(RELAY_LIGHT, LOW);
  digitalWrite(RELAY_HEATER_RED, LOW);
  digitalWrite(RELAY_HEATER_BLACK, LOW);
}

void saveConfig() {
  DynamicJsonDocument doc(256);
  doc["ssid"] = ssid;
  doc["password"] = password;
  doc["targetTemp"] = targetTemp;
  doc["sensor1Offset"] = sensor1Offset;
  doc["sensor2Offset"] = sensor2Offset;
  doc["heaterDisabled"] = heaterDisabled;
  File f = SPIFFS.open("/config.json", "w");
  serializeJson(doc, f);
  f.close();
}

void loadConfig() {
  if (!SPIFFS.begin(true)) return;
  if (SPIFFS.exists("/config.json")) {
    File f = SPIFFS.open("/config.json", "r");
    DynamicJsonDocument doc(256);
    deserializeJson(doc, f);
    if (doc.containsKey("ssid")) {
        ssid = doc["ssid"].as<String>();
        Serial.print("[INFO]: SSID updated: ");
        Serial.println(ssid);
      }
    if (doc.containsKey("password")) {
        password = doc["password"].as<String>();
        Serial.print("[INFO]: Password updated.");
      }
    float uiTarget = doc["targetTemp"].as<float>();
      if (uiTarget >= 10.0 && uiTarget <= 41.0) {
        targetTemp = uiTarget;
      } else {
        Serial.println("[WARN]: Target temperature from UI is out of range (10.0 - 41.0 °C), ignoring update.");
      }
    if (doc.containsKey("sensor1Offset")) sensor1Offset = doc["sensor1Offset"].as<float>();
    if (doc.containsKey("sensor2Offset")) sensor2Offset = doc["sensor2Offset"].as<float>();
    if (doc.containsKey("heaterDisabled")) heaterDisabled = doc["heaterDisabled"].as<bool>();
    f.close();
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Hot Tub Controller</title>";
  html += "<style>";
  html += "button{padding:10px;margin:5px;}button.on{background-color:#f4a460;}";
  html += "table, th, td {border: 1px solid black;border-collapse: collapse;}th, td {padding: 15px;} tr {border-bottom: 1px solid #ddd;}tr:hover {background-color: #D6EEEE;}";
  html += "</style>";
  html += "<script>";
  html += "function send(cmd){fetch('/'+cmd).then(()=>setTimeout(()=>location.reload(),500));}";
  html += "function saveCfg(){const tempInput = document.getElementById('targetTemp').value;const parsedTemp = parseFloat(tempInput);if (parsedTemp < 10.0 || parsedTemp > 41.0) {alert('Target temperature must be between 10.0 and 41.0 °C');document.getElementById('targetTemp').style.borderColor = 'red';return;} else {document.getElementById('targetTemp').style.borderColor = '';}const d={ssid:document.getElementById('ssid').value,password:document.getElementById('password').value,targetTemp:parsedTemp};fetch('/saveCfg',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).then(()=>setTimeout(()=>location.reload(),1000));}";
  html += "function updateStatus() {";
  html += "fetch('/status.html?action=status')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('currentTemp').innerText = data.current_temp;";
  html += "document.getElementById('targetDisplay').innerText = data.target_temp;";
  html += "document.getElementById('sensorTemp2').innerText = data.temp2;";
  html += "document.getElementById('sensorDelta').innerText = data.delta;";
  
  html += "let heaterStatus = data.heaterDisabled ? 'DISABLED' : (parseFloat(data.current_temp) < parseFloat(data.target_temp) && !data.overheat ? 'ON' : 'OFF');";
  html += "document.getElementById('heaterStatus').innerText = heaterStatus;";
  html += "document.getElementById('heaterStatus').style.backgroundColor = (heaterStatus === 'ON') ? 'orange' : (heaterStatus === 'DISABLED' ? 'gray' : '');";
  html += "let lastUpdateTime = Date.now();";
  html += "setInterval(() => { const ago = Math.floor((Date.now() - lastUpdateTime) / 1000); document.getElementById('updateTime').innerText = (ago < 10 ? '0' : '') + ago + ' seconds ago'; }, 1000);";
  html += "lastUpdateTime = Date.now();";
  html += "const p1Button = document.querySelector(\"button[onclick*=pump1lo]\");";
  html += "const p2Button = document.querySelector(\"button[onclick*=pump2]\");";
  html += "const p3Button = document.querySelector(\"button[onclick*=pump3]\");";
  html += "const lightButton = document.querySelector(\"button[onclick*=light]\");";
  html += "p1Button.innerText = 'Pump1 ' + data.pump1.charAt(0).toUpperCase() + data.pump1.slice(1);";
  html += "p1Button.className = data.pump1 !== 'off' ? 'on' : '';";
  html += "p2Button.className = data.pump2 === 'on' ? 'on' : '';";
  html += "p3Button.className = data.pump3 === 'on' ? 'on' : '';";
  html += "lightButton.className = data.lights === 'on' ? 'on' : '';";
  html += "if (data.overheat) { document.getElementById('warning').style.display = 'block'; } else { document.getElementById('warning').style.display = 'none'; }";
  html += "});";
  html += "}";
  html += "setInterval(updateStatus, 10000);";
  html += "</script></head><body>";
  html += "<h1>Hot Tub Controller</h1>";
  html += "<p>version: " + String(version) + "</p>";
  html += "<p id=\"warning\" style=\"color:red;display:";
  html += overheat ? "block" : "none";
  html += ";\">WARNING: OVERHEAT</p>";
  html += "<table>";
  html += "<tr><td  style='width:120'>Input Temperature: </td><td style='width:60'><span id='currentTemp'>" + String(currentTemp) + "</td><td style='width:30'> &deg;C</span></td><tr>";
  html += "<tr><td>Output Temperature: </td><td><span id='sensorTemp2'>" + String(sensor2Temp) + " </td><td>&deg;C</span></td><tr>";
  html += "<tr><td>Target Temperature: </td><td><span id='targetDisplay'>" + String(targetTemp) + " </td><td>&deg;C</span></td><tr>"; 
  html += "<tr><td>Heater Status: </td><td><span id='heaterStatus' style='padding:5px;'>" + String(heaterDisabled ? "DISABLED" : (heaterOn ? "ON" : "OFF")) + "</span></td><td></td><tr>";
  html += "<tr><td>Sensor Delta: </td><td><span id='sensorDelta'>--</span></td><td> &deg;C</td><tr>";
  html += "</table><br>";

  bool p1lo = digitalRead(RELAY_PUMP1LO);
  bool p1hi = digitalRead(RELAY_PUMP1HI);
  bool p2 = digitalRead(RELAY_PUMP2);
  bool p3 = digitalRead(RELAY_PUMP3);
  bool light = digitalRead(RELAY_LIGHT);

  String p1status = p1hi ? "High" : (p1lo ? "Low" : "Off");
  html += "<button onclick=\"send('pump1lo')\" class='";
  html += p1lo ? "on" : "";
  html += "'>Pump1 " + p1status + "</button> ";
  html += "<button onclick=\"send('pump2')\" class='";
  html += p2 ? "on" : "";
  html += "'>Pump2</button> ";
  html += "<button onclick=\"send('pump3')\" class='";
  html += p3 ? "on" : "";
  html += "'>Pump3</button> ";
  html += "<button onclick=\"send('light')\" class='";
  html += light ? "on" : "";
  html += "'>Toggle Light</button><br>";
  html += "<p>Last update: <span id='updateTime'>-</span></p><hr>";
  html += "<h3>WiFi and Temperature Settings</h3>SSID: <input id='ssid' value='" + ssid + "'><br>";
  html += "Password: <input id='password' type='password' value='" + password + "'><br>";
  html += "Target Temp: <input id='targetTemp' value='" + String(targetTemp) + "'><br>";
  html += "<button onclick=\"saveCfg()\">Save</button><hr>";

  html += "<h3>Send Serial Command</h3>";
  html += "<input type='text' id='serialCmd' placeholder='e.g., settargettemperature 37.0' size='50'><br>";
  html += "<button onclick=\"sendSerial()\">Send</button>";
  //-------------------------------------------------------------------------------
  html += "";
  html += "<hr>";
  html += "<h3>Firmware Update</h3>";
  html += "<button onclick=\"sendUpdate('checkupdate')\">Check for Update</button>";
  html += "<button onclick=\"sendUpdate('firmwareupgrade')\">Upgrade Firmware</button>";
  html += "<div id=\"progressContainer\" style=\"width: 100%; background-color: #ddd; margin-top: 10px; display:none;\">";
  html += "<div id=\"progressBar\" style=\"width: 0%; height: 30px; background-color: #4CAF50;\"></div>";
  html += "</div>";
  html += "<p id=\"progressText\"></p>";
  //-------------------------------------------------------------------------------
  html += "<script>";
  html += "function sendUpdate(cmd) {";
  html += "document.getElementById('progressContainer').style.display = 'block';";
  html += "document.getElementById('progressBar').style.width = '0%';";
  html += "document.getElementById('progressText').innerText = \"Starting...\";";
  html += "fetch('/serial', {";
  html += "method: 'POST',";
  html += "headers: { 'Content-Type': 'application/json' },";
  html += "body: JSON.stringify({ command: cmd })";
  html += "})";
  html += "  .then(res => {";
  html += "let reader = res.body.getReader();";
  html += "let received = 0;";
  html += "let contentLength = +res.headers.get('Content-Length') || 1000000; // Fake size if not provided";
  html += "function pump() {";
  html += "return reader.read().then(({ done, value }) => {";
  html += "if (done) {";
  html += "document.getElementById('progressBar').style.width = '100%';";
  html += "document.getElementById('progressText').innerText = \"Update Finished.\";";
  html += "setTimeout(() => location.reload(), 2000);";
  html += "return;";
  html += "}";
  html += "received += value.length;";
  html += "let percent = Math.floor(received / contentLength * 100);";
  html += "if (percent > 100) percent = 100;";
  html += "document.getElementById('progressBar').style.width = percent + '%';";
  html += "document.getElementById('progressText').innerText = \"Updating: \" + percent + \"%\";";
  html += "return pump();";
  html += "});";
  html += "}";
  html += "return pump();";
  html += "})";
  html += ".catch(err => {";
  html += "  document.getElementById('progressText').innerText = \"Error: \" + err;";
  html += "});";
  html += "}";
  html += "function sendSerial() {";
  html += "  const cmd = document.getElementById('serialCmd').value;";
  html += "  fetch('/serial', {";
  html += "    method: 'POST',";
  html += "    headers: { 'Content-Type': 'application/json' },";
  html += "    body: JSON.stringify({ command: cmd })";
  html += "  }).then(res => res.text()).then(text => alert(text));";
  html += "}";
  html += "</script>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Helper function
int convertMonth(String m) {
  if (m == "Jan") return 1;
  if (m == "Feb") return 2;
  if (m == "Mar") return 3;
  if (m == "Apr") return 4;
  if (m == "May") return 5;
  if (m == "Jun") return 6;
  if (m == "Jul") return 7;
  if (m == "Aug") return 8;
  if (m == "Sep") return 9;
  if (m == "Oct") return 10;
  if (m == "Nov") return 11;
  if (m == "Dec") return 12;
  return 0;
}

void handleSaveCfg() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      bool updated = false;

      if (doc.containsKey("ssid")) {
        ssid = doc["ssid"].as<String>();
        Serial.print("[INFO]: SSID updated: ");
        Serial.println(ssid);
        updated = true;
      }

      if (doc.containsKey("password")) {
        password = doc["password"].as<String>();
        Serial.println("[INFO]: Password updated.");
        updated = true;
      }

      if (doc.containsKey("targetTemp")) {
        float temp = doc["targetTemp"].as<float>();
        if (temp >= 10.0 && temp <= 41.0) {
          targetTemp = temp;
          Serial.print("[INFO]: Target temperature updated from UI: ");
          Serial.println(targetTemp);
          updated = true;
        } else {
          Serial.println("[WARN]: Target temperature from UI is out of range (10.0 - 41.0 °C), ignoring update.");
        }
      }

      if (updated) saveConfig();
      server.send(200, "text/plain", "Saved");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleCommand(String cmd) {
  if (cmd == "pump1lo") {
    bool lo = digitalRead(RELAY_PUMP1LO);
    bool hi = digitalRead(RELAY_PUMP1HI);

    if (heaterOn) {
      // Heater is ON → only allow toggling between LOW and HIGH
      if (lo && !hi) {
        digitalWrite(RELAY_PUMP1LO, LOW);
        digitalWrite(RELAY_PUMP1HI, HIGH);
      } else {
        digitalWrite(RELAY_PUMP1LO, HIGH);
        digitalWrite(RELAY_PUMP1HI, LOW);
      }
    } else {
      // Heater is OFF → cycle through OFF → LOW → HIGH → OFF
      if (!lo && !hi) {
        digitalWrite(RELAY_PUMP1LO, HIGH);
        digitalWrite(RELAY_PUMP1HI, LOW);
      } else if (lo && !hi) {
        digitalWrite(RELAY_PUMP1LO, LOW);
        digitalWrite(RELAY_PUMP1HI, HIGH);
      } else {
        digitalWrite(RELAY_PUMP1LO, LOW);
        digitalWrite(RELAY_PUMP1HI, LOW);
      }
    }

    pump1Running = digitalRead(RELAY_PUMP1LO) || digitalRead(RELAY_PUMP1HI);
    if (pump1Running) pump1StartTime = millis();

  } else if (cmd == "pump2") {
    digitalWrite(RELAY_PUMP2, !digitalRead(RELAY_PUMP2));
  } else if (cmd == "pump3") {
    digitalWrite(RELAY_PUMP3, !digitalRead(RELAY_PUMP3));
  } else if (cmd == "light") {
    digitalWrite(RELAY_LIGHT, !digitalRead(RELAY_LIGHT));
  }

  beep();
  server.send(200, "text/plain", "OK");
}

void setupWiFi() {
  // Set hostname based on last 4 digits of MAC
  WiFi.mode(WIFI_STA); 
  WiFi.begin(ssid.c_str(), password.c_str());
  delay(100);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.printf("[DEBUG]: MAC Address: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  char hostname[20];
  snprintf(hostname, sizeof(hostname), "HotTub_%02X%02X", mac[4], mac[5]);
  WiFi.setHostname(hostname);
  String connecting = "\nConnecting to WiFi: \"";
  connecting += ssid;
  connecting += "\"";
  Serial.println(connecting);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 30000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected to WiFi. IP address: ");
    Serial.println(WiFi.localIP());
    beepDouble();
    return;
  }
else {
  Serial.println("\n[WARN]: Failed to connect to WiFi, starting AP mode...");
  WiFi.softAP(hostname, "admin1234");
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("[INFO]: DNS server started for captive portal.");
  Serial.print("SSID: ");
  Serial.println(hostname);
  Serial.println("PWD:  admin1234");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  beepTripleSlow();
  return;
  }
}

void handleStatus() {
  DynamicJsonDocument doc(256);
  doc["current_temp"] = String(currentTemp, 1);
  doc["target_temp"] = String(targetTemp, 1);
  doc["temp2"] = String(sensor2Temp, 1);
  doc["delta"] = String(currentTemp - sensor2Temp, 1);
  doc["pump1"] = digitalRead(RELAY_PUMP1HI) ? "high" : (digitalRead(RELAY_PUMP1LO) ? "low" : "off");
  doc["pump2"] = digitalRead(RELAY_PUMP2) ? "on" : "off";
  doc["pump3"] = digitalRead(RELAY_PUMP3) ? "on" : "off";
  doc["lights"] = digitalRead(RELAY_LIGHT) ? "on" : "off";
  String statusString;
  if (heaterDisabled) {
   statusString = "DISABLED";
  } else if (heaterOn) {
   statusString = "ON";
  } else {
   statusString = "OFF";
  }
  doc["heaterStatus"] = statusString;
  doc["heaterDisabled"] = heaterDisabled;
  doc["overheat"] = overheat;
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void setupWebServer() {
  server.on("/serial", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      DynamicJsonDocument doc(128);
      DeserializationError error = deserializeJson(doc, server.arg("plain"));
      if (!error && doc.containsKey("command")) {
        String cmd = doc["command"].as<String>();
        processCommand(cmd);
        server.send(200, "text/plain", "Command sent to serial processor");
        return;
      }
    }
    server.send(400, "text/plain", "Invalid command");
  });
  server.on("/", handleRoot);
  server.on("/pump1lo", []() { handleCommand("pump1lo"); });
  server.on("/pump2", []() { handleCommand("pump2"); });
  server.on("/pump3", []() { handleCommand("pump3"); });
  server.on("/light", []() { handleCommand("light"); });
  server.on("/saveCfg", HTTP_POST, handleSaveCfg);
    server.on("/status.html", []() {
    if (server.hasArg("action") && server.arg("action") == "status") {
      handleStatus();
    } else {
      server.send(400, "text/plain", "Missing or invalid action parameter");
    }
  });
  server.begin();
}

void readTemperatures(float &temp1, float &temp2, float &delta) {
  int samples1 = 0;
  int samples2 = 0;

  for (int i = 0; i < 5; i++) {  // Take 5 samples
    samples1 += analogRead(TEMP_SENSOR_1);
    samples2 += analogRead(TEMP_SENSOR_2);
    delay(2); // small pause between reads
  }
  
  float analogVal1 = samples1 / 5.0;
  float analogVal2 = samples2 / 5.0;

  float voltage1 = analogVal1 / 4095.0 * 3.3;
  float resistance1 = voltage1 * 10000 / (3.3 - voltage1);
  temp1 = 1 / (log(resistance1 / 10000) / 3950 + 1.0 / 298.15) - 273.15 + sensor1Offset;

  float voltage2 = analogVal2 / 4095.0 * 3.3;
  float resistance2 = voltage2 * 10000 / (3.3 - voltage2);
  temp2 = 1 / (log(resistance2 / 10000) / 3950 + 1.0 / 298.15) - 273.15 + sensor2Offset;

  delta = temp1 - temp2;
}


void checkHeaterLogic() {
  float delta = 0;
  readTemperatures(currentTemp, sensor2Temp, delta);
  overheat = currentTemp > 46.0;

  if (overheat) {
    heaterOn = false;
    heaterDisabled = true;
// Ensure Pump1 runs on LOW during overheat
  if (!digitalRead(RELAY_PUMP1LO)) {
    digitalWrite(RELAY_PUMP1LO, HIGH);
    digitalWrite(RELAY_PUMP1HI, LOW);
    pump1Running = true;
    pump1StartTime = millis();
    }
 // Blink lights while in overheat
    if (!blinkingLights) {
     overheatLightTicker.attach(2.0, toggleLightsDuringOverheat);  // blink every 2 seconds
     blinkingLights = true;
    }
  }
  else {
    overheatLightTicker.detach();
    blinkingLights = false;
    digitalWrite(RELAY_LIGHT, lightRequested ? HIGH : LOW);
    }

  if (!heaterOn && !heaterDisabled && currentTemp < (targetTemp - 1.0) && !overheat) {
    if (!pump1Running) {
      digitalWrite(RELAY_PUMP1LO, HIGH);
      pump1StartTime = millis();
      pump1Running = true;
    }
    if (millis() - pump1StartTime >= 10000) {
      digitalWrite(RELAY_HEATER_RED, HIGH);
      digitalWrite(RELAY_HEATER_BLACK, HIGH);
      heaterOn = true;
    }
  } else if (heaterOn && (currentTemp >= (targetTemp - 0.5) || overheat)) {
    digitalWrite(RELAY_HEATER_RED, LOW);
    digitalWrite(RELAY_HEATER_BLACK, LOW);
    heaterOn = false;
    heaterCooldownStart = millis();
  }

  if (!heaterOn && heaterCooldownStart > 0 && millis() - heaterCooldownStart >= 30000) {
    digitalWrite(RELAY_PUMP1LO, LOW);
    digitalWrite(RELAY_PUMP1HI, LOW);
    pump1Running = false;
    heaterCooldownStart = 0;
  }
}

void toggleLightsDuringOverheat() {
  bool current = digitalRead(RELAY_LIGHT);
  digitalWrite(RELAY_LIGHT, !current);
  beep();
}


void processCommand(String instruction) {
  instruction.trim();
  String command = getValue(instruction, ' ', 0);
  String attribute = getValue(instruction, ' ', 1);
  attribute.trim();

  Serial.print("[INFO]: Command: ");
  Serial.println(command);
  Serial.print("[INFO]: Attribute: ");
  Serial.println(attribute);

  if (command == "reboot") {
    ESP.restart();
  } else if (command == "setwifiname") {
    Serial.print("WiFi name updated: ");
    ssid = attribute;
    saveConfig();
  } else if (command == "setwifipassword") {
    Serial.print("WiFi password updated: ");
    password = attribute;
    saveConfig();
  } else if (command == "connectwifi") {
    Serial.println("Re-connecting WiFi:");
    setupWiFi();
  } else if (command == "calibratesensor1") {
    float measured = attribute.toFloat();
    float raw;
    int analogVal = analogRead(TEMP_SENSOR_1);
    float voltage = analogVal / 4095.0 * 3.3;
    float resistance = voltage * 10000 / (3.3 - voltage);
    raw = 1 / (log(resistance / 10000) / 3950 + 1.0 / 298.15) - 273.15;
    sensor1Offset = measured - raw;
    Serial.print("Sensor 1 offset calculated as: ");
    Serial.println(sensor1Offset);
    saveConfig();
  } else if (command == "calibratesensor2") {
    float measured = attribute.toFloat();
    float raw;
    int analogVal = analogRead(TEMP_SENSOR_2);
    float voltage = analogVal / 4095.0 * 3.3;
    float resistance = voltage * 10000 / (3.3 - voltage);
    raw = 1 / (log(resistance / 10000) / 3950 + 1.0 / 298.15) - 273.15;
    sensor2Offset = measured - raw;
    Serial.print("Sensor 2 offset calculated as: ");
    Serial.println(sensor2Offset);
    saveConfig();
    }
  else if (command == "settargettemperature") {
    float newTemp = attribute.toFloat();
    if (newTemp >= 10.0 && newTemp <= 41.0) {
      targetTemp = newTemp;
      Serial.print("Target temperature updated: ");
      Serial.println(targetTemp);
      saveConfig();
    } else {
      Serial.println("[ERROR]: Target temperature out of range (10.0 - 41.0 °C)");
    }
  }
  else if (command == "disableheater") {
    if (attribute == "on") {
      heaterDisabled = true;
      Serial.println("[INFO]: Heater has been DISABLED.");
    } else if (attribute == "off") {
      heaterDisabled = false;
      Serial.println("[INFO]: Heater has been ENABLED.");
    } else {
      Serial.println("[ERROR]: Invalid attribute. Use 'on' or 'off'.");
    }
    saveConfig();
  }
  
  else if (command == "lights") {
    if (attribute == "on") {
      lightRequested = true;
      digitalWrite(RELAY_LIGHT, HIGH);
      Serial.println("[INFO]: Lights turned ON.");
    } 
    else if (attribute == "off") {
      digitalWrite(RELAY_LIGHT, LOW);
      Serial.println("[INFO]: Lights turned OFF.");
    } 
    else {
      Serial.println("[ERROR]: Invalid attribute. Use 'on' or 'off'.");
    }
  }
else if (command == "checkupdate") {
  HTTPClient http;
  Serial.print("[DEBUG]: Checking URL: ");
  Serial.println(latestVersionURL);

  http.begin(latestVersionURL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // <-- ADD THIS LINE
  int httpCode = http.GET();
  if (httpCode == 200) {
    String latestVersion = http.getString();
    latestVersion.trim();
    Serial.print("[INFO]: Latest available version: ");
    Serial.println(latestVersion);
    Serial.print("[INFO]: Current firmware version: ");
    Serial.println(version);

    if (latestVersion != version) {
      Serial.println("[UPDATE]: Newer version available!");
    } else {
      Serial.println("[UPDATE]: Already up to date.");
    }

    // OPTIONAL SMARTER LOGIC
    if (latestVersion.length() >= 20 && version.length() >= 20) {
      int latestYear = latestVersion.substring(7, 11).toInt();
      int latestMonth = convertMonth(latestVersion.substring(0, 3));
      int latestDay = latestVersion.substring(4, 6).toInt();

      int localYear = version.substring(7, 11).toInt();
      int localMonth = convertMonth(version.substring(0, 3));
      int localDay = version.substring(4, 6).toInt();

      if (latestYear > localYear ||
          (latestYear == localYear && latestMonth > localMonth) ||
          (latestYear == localYear && latestMonth == localMonth && latestDay > localDay)) {
        Serial.println("[UPDATE]: Newer firmware available based on build date!");
      } else {
        Serial.println("[UPDATE]: Current firmware is newer or same.");
      }
    }
  } else {
    Serial.print("[ERROR]: Failed to fetch latest version. HTTP code: ");
    Serial.println(httpCode);
  }
  http.end();
}
else if (command == "firmwareupgrade") {
  Serial.println("[UPDATE]: Starting firmware upgrade...");
  WiFiClient client;
  t_httpUpdate_return ret = httpUpdate.update(client, firmwareURL);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[UPDATE ERROR]: %s\n", httpUpdate.getLastErrorString().c_str());
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[UPDATE]: No updates found.");
      break;

    case HTTP_UPDATE_OK:
      Serial.println("[UPDATE]: Update successful, rebooting...");
      ESP.restart();
      break;
  }
}

  
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17);
  pinMode(SPEAKER_PIN, OUTPUT);
  setupRelays();
  loadConfig();
  startupSound();
  setupWiFi();
  setupWebServer();
  checkHeaterLogic();
  // OTA updates
  ArduinoOTA
  .onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";
    Serial.println("[OTA]: Start updating " + type);
  })
  .onEnd([]() {
    Serial.println("\n[OTA]: Update finished. Rebooting...");
    delay(500);  // Give time for the Serial message to go out
    ESP.restart();
  })
  .onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA]: Progress: %u%%\r", (progress / (total / 100)));
  })
  .onError([](ota_error_t error) {
    Serial.printf("[OTA ERROR]: %u\n", error);
    if (error == OTA_AUTH_ERROR) Serial.println("[OTA]: Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("[OTA]: Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("[OTA]: Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("[OTA]: Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("[OTA]: End Failed");
  });

ArduinoOTA.setHostname("HotTubOTA"); // Optional: set custom OTA hostname
MDNS.begin("HotTubOTA");
//ArduinoOTA.setPassword("htpassword");
ArduinoOTA.begin();
Serial.println("[OTA]: Ready");
}

unsigned long lastLogicCheck = 0;
const unsigned long logicInterval = 500;

void loop() {
  dnsServer.processNextRequest();
  ArduinoOTA.handle();
  server.handleClient();
  unsigned long now = millis();
// Check commands over Serial (USB)
if (Serial.available()) {
  String input = Serial.readStringUntil('\n');
  processCommand(input);
}

// Check commands over Serial2 (UART2 - RX2/TX2)
if (Serial2.available()) {
  String input2 = Serial2.readStringUntil('\n');
  processCommand(input2);
}
  if (now - lastLogicCheck >= logicInterval) {
  lastLogicCheck = now;

  float t1, t2, delta;
  readTemperatures(t1, t2, delta);
  if (t1 > 45.0) {  // Emergency overheat immediate response
    if (!overheat) {
      Serial.println("[EMERGENCY]: Instant Overheat Detected!");
      overheat = true;
      heaterOn = false;
      heaterDisabled = true;

      digitalWrite(RELAY_HEATER_RED, LOW);
      digitalWrite(RELAY_HEATER_BLACK, LOW);

      if (!digitalRead(RELAY_PUMP1LO)) {
        digitalWrite(RELAY_PUMP1LO, HIGH);
        digitalWrite(RELAY_PUMP1HI, LOW);
        pump1Running = true;
        pump1StartTime = millis();
      }
      if (!blinkingLights) {
        overheatLightTicker.attach(2.0, toggleLightsDuringOverheat);
        blinkingLights = true;
      }
    }
  }
  temp1Sum += t1;
  temp2Sum += t2;
  tempSamples++;
}

// Every 10 seconds, calculate average and run heater logic
if (now - lastAverageTime >= 10000) {
  if (tempSamples > 0) {
    averagedTemp1 = temp1Sum / tempSamples;
    averagedTemp2 = temp2Sum / tempSamples;
  }

  temp1Sum = 0;
  temp2Sum = 0;
  tempSamples = 0;
  lastAverageTime = now;

  // Update global temperatures based on averages
  currentTemp = averagedTemp1;
  sensor2Temp = averagedTemp2;

  // Now use averaged temperatures to check heater logic
  checkHeaterLogic();
  }
}

// Future functions
void syncWithTouchPanel() {}
void handleOverheatWarning() {}
void controlHeater() {}
void updateTemperatureDisplay() {}
void sendStatusToPanel() {}
void autoControlLoop() {}
