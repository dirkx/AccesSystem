/* Voordeur hal 'as is' configuration which should be near identical
    to the existing setup late 2017.

*/
// #include "/Users/dirkx/.passwd.h"
#include "MFRC522.h"

// Wired ethernet.
//
#define ETH_PHY_ADDR      1
#define ETH_PHY_MDC       23
#define ETH_PHY_MDIO      18
#define ETH_PHY_POWER     17
#define ETH_PHY_TYPE      ETH_PHY_LAN8720

// Labelling as per `blue' RFID MFRC522-MSL 1471 'fixed'
//
#define MFRC522_SDA     (15)
#define MFRC522_SCK     (14)
#define MFRC522_MOSI    (13)
#define MFRC522_MISO    (12)
#define MFRC522_IRQ     (33)
#define MFRC522_GND     /* gnd pin */
#define MFRC522_RSTO    (32)
#define MFRC522_3V3     /* 3v3 */

#define SOLENOID_GPIO     (4)
#define SOLENOID_OFF      (LOW)
#define SOLENOID_ENGAGED  (HIGH)

#define AARTLED_GPIO      (16)

#define WIRECHECK_GPIO	  (5)
#define WIRECHECK_REDLED  (HIGH)

#define DOOR_OPEN_DELAY         (5*1000)   //  10 seconds -- how long to hold the relay engaged; in milli seconds.
#define CACHE_FALLBACK_TIMEOUT  (500)       // 0.5 second  -- how long to wait for a reply before checking the cache.
#define MAX_RESPONSE_TIMEOUT    (3500)      // 3.5 seconds -- max wait for reply from the server.

// Report regulary - but swithc to a higher frequency if we are in test mode. Also prefix our MQTT topic with 'test'
// to avoid confusing production.
//
#ifdef LOCALMQTT
#define REPORTING_PERIOD        (300*1000)  //   5 minutes -- how often to report our states; in milli seconds.
#define PREFIX ""
#else
#define REPORTING_PERIOD        (10*1000)   //  10 seconds -- -- how often to report our states; in milli seconds.
#define PREFIX "test"
#define MQTT_KEEPALIVE          (10)        // seconds -- keep alive check; default is 2 minutes - but it is 
// easier to keep this short during debugging - i.e. when testing breaking connections. Note that this may not
// work if the Arduino CPP gets the order wrong.
#endif

typedef enum doorstates { CLOSED, CHECKINGCARD, OPEN } doorstate_t;
const char doorstates_names[OPEN + 1][15] = { "closed", "checking-card", "open" };

doorstate_t doorstate;
unsigned long long last_doorstatechange = 0;

long cnt_cards = 0, cnt_opens = 0, cnt_closes = 0, cnt_fails = 0, cnt_misreads = 0, cnt_minutes = 0, cnt_reconnects = 0, cnt_mqttfails = 0, cnt_failed_rfid = 0;

#include <ETH.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <FS.h>

#include <ArduinoOTA.h>
#include <ESP32Ticker.h>
#include <PubSubClient.h>

// Requires modifed MFRC522 (see pull rq) or the -master branch as of late DEC 2017.
// https://github.com/miguelbalboa/rfid.git
#include <MFRC522.h>

SPIClass spirfid = SPIClass(VSPI);
const SPISettings spiSettings = SPISettings(SPI_CLOCK_DIV4, MSBFIRST, SPI_MODE0);
MFRC522 mfrc522(MFRC522_SDA, MFRC522_RSTO, &spirfid, spiSettings);

Ticker solenoid;

#ifdef LOCALMQTT
#warning "Production build"
const IPAddress mqtt_host = LOCALMQTT;
#else
const char mqtt_host[] = "space.vijn.org";
#endif
const unsigned short mqtt_port = 1883;

extern void callback(char* topic, byte * payload, unsigned int length);

bool caching = false;

#ifdef LOCALMQTT
#define PREFIX ""
#else
#define PREFIX "test"
#endif

const char rfid_topic[] = PREFIX "deur/voordeur/rfid";
const char door_topic[] = PREFIX "deur/voordeur/open";
const char log_topic[] = PREFIX "log";

WiFiClient wifiClient;
PubSubClient client(wifiClient);

static bool eth_connected = false;

char pname[128] = "some-unconfigured-door";

static bool ota = false;
void enableOTA() {
  if (ota)
    return;

  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname(pname);
#ifdef LOCALOTA
  ArduinoOTA.setPasswordHash(LOCALOTA);
#endif

  ArduinoOTA.onStart([]() {
    String type;
    switch (ArduinoOTA.getCommand()) {
      case U_FLASH:
        type = "Firmware";
        break;
      case U_SPIFFS:
        type = "SPIFFS";
        break;
      default: {
          char buff[255];
          snprintf(buff, sizeof(buff), "[%s] Unknown type of reprogramming attempt. Rejecting.", pname);
          Serial.println(buff);
          client.publish(log_topic, buff);
          client.loop();
          ESP.restart();
        }
        break;
    };
    char buff[256];
    snprintf(buff, sizeof(buff), "[%s] %s OTA reprogramming started.", pname, type.c_str());

    Serial.println(buff);
    client.publish(log_topic, buff);
    client.loop();

    closeDoor();

    SPIFFS.end();
  });

  ArduinoOTA.onEnd([]() {
    char buff[256];
    snprintf(buff, sizeof(buff), "[%s] OTA re-programming completed. Rebooting.", pname);

    Serial.println(buff);
    client.publish(log_topic, buff);
    client.loop();

    client.disconnect();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int lp = -1 ;
    int p = progress / (total / 10);
    if (p != lp) Serial.printf("Progress: %u %%\n", p * 10);
    lp = p;
    digitalWrite(AARTLED_GPIO, ((millis() >> 7) & 3) == 0);
  });

  // Unfortunately-deep in OTA it auto defaults to Wifi. So we
  // force it to ETH -- requires pull RQ https://github.com/espressif/arduino-esp32/issues/944
  // and https://github.com/espressif/esp-idf/issues/1431.
  //
  // ArduinoOTA.begin(TCPIP_ADAPTER_IF_ETH);
  ArduinoOTA.begin();

  Serial.println("OTA enabled.");
  ota = true;
}

String DisplayIP4Address(IPAddress address)
{
  return String(address[0]) + "." +
         String(address[1]) + "." +
         String(address[2]) + "." +
         String(address[3]);
}

String connectionDetailsString() {
  return "Wired Ethernet: " + ETH.macAddress() +
         ", IPv4: " + DisplayIP4Address(ETH.localIP()) + ", " +
         ((ETH.fullDuplex()) ? "full" : "half") + "-duplex, " +
         String(ETH.linkSpeed()) + " Mbps, Build: " +
         String(__DATE__) + " " + String(__TIME__);
}

void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {
    case SYSTEM_EVENT_ETH_START:
      Serial.println("ETH Started" + ETH.macAddress());
      ETH.setHostname(pname);
      break;
    case SYSTEM_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected " + ETH.macAddress());
      break;
    case SYSTEM_EVENT_ETH_GOT_IP:
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.println(connectionDetailsString());
      eth_connected = true;
      break;
    case SYSTEM_EVENT_ETH_DISCONNECTED:
      Serial.printf("ETH Disconnected (event %d)\n", event);
      eth_connected = false;
      break;
    case SYSTEM_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      break;
    default:
      Serial.printf("Unknown event %d\n", event);
      break;
  }
}

volatile boolean irqCardSeen = false;

void readCard() {
  irqCardSeen = true;
}

/* The function sending to the MFRC522 the needed commands to activate the reception
*/
void activateRec(MFRC522 mfrc522) {
  mfrc522.PCD_WriteRegister(mfrc522.FIFODataReg, mfrc522.PICC_CMD_REQA);
  mfrc522.PCD_WriteRegister(mfrc522.CommandReg, mfrc522.PCD_Transceive);
  mfrc522.PCD_WriteRegister(mfrc522.BitFramingReg, 0x87);
}

/*  The function to clear the pending interrupt bits after interrupt serving routine
*/
void clearInt(MFRC522 mfrc522) {
  mfrc522.PCD_WriteRegister(mfrc522.ComIrqReg, 0x7F);
}

static long lastReconnectAttempt = 0;

boolean reconnect() {
  if (!client.connect(pname)) {
    // Do not log this to the MQTT bus-as it may have been us posting too much
    // or some other loop-ish thing that triggered our disconnect.
    //
    Serial.println("Failed to reconnect to MQTT bus.");
    return false;
  }

  char buff[256];
  snprintf(buff, sizeof(buff), "[%s] %sconnected, %s", pname, cnt_reconnects ? "re" : "", connectionDetailsString().c_str());
  client.publish(log_topic, buff);
  client.subscribe(door_topic);
  Serial.println(buff);

  cnt_reconnects++;

  return client.connected();
}

void reinitRFID() {
  Serial.println("MFRC522 IRQ and callback setup.");

  mfrc522.PCD_Init();   // Init MFRC522
  // mfrc522.PCD_DumpVersionToSerial();  // Show details of PCD-MFRC522 Card Reader details

  pinMode(MFRC522_IRQ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(MFRC522_IRQ), readCard, FALLING);

  byte regVal = 0xA0; //rx irq
  mfrc522.PCD_WriteRegister(mfrc522.ComIEnReg, regVal);
}
void setup()
{
  const char * f = __FILE__;
  char * p = rindex(f, '/');
  if (p)
    strncpy(pname, p + 1, sizeof(pname));

  if ((p = index(pname, '.')))
    * p = '\0';

#ifndef LOCALMQTT
  // Alert us to safe, non production versions.
  strncat(pname, "-test", sizeof(pname));
#endif

  Serial.begin(115200);
  Serial.print("\n\n\n\nStart ");
  Serial.print(pname);
  Serial.println(" -- buuld: " __DATE__ " " __TIME__ );

  pinMode(SOLENOID_GPIO, OUTPUT);
  digitalWrite(SOLENOID_GPIO, SOLENOID_OFF);

  pinMode(AARTLED_GPIO, OUTPUT);
  digitalWrite(AARTLED_GPIO, HIGH);

  WiFi.onEvent(WiFiEvent);
  if (!(ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC,
                  ETH_PHY_MDIO, ETH_PHY_TYPE
#ifdef ETH_CLK_MODE
                  , ETH_CLK_MODE
#endif
                 ))) {
    Serial.println("Ethernet failed to begin() up\n");
  }

  Serial.println("SPI init");
  spirfid.begin(MFRC522_SCK, MFRC522_MISO, MFRC522_MOSI, MFRC522_SDA);

  reinitRFID();
  Serial.println("Setting up MQTT to " + String(mqtt_host)  + ":" + String(mqtt_port, DEC));
  client.setServer(mqtt_host, mqtt_port);
  client.setCallback(callback);

  doorstate = CLOSED;
  last_doorstatechange = millis();

  Serial.println("SPIFF setup");
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS Mount Failed-reformatting");
    wipeCache();
  } else {
    caching = true;
    unsigned long count = listDir(SPIFFS, "/", "");
    Serial.println("Caching on - Total of " + String(count, DEC) + " files.");
  };

  Serial.println("setup() done.\n\n");
}

// This function is taken from the SPIFFS_Test example. Under the expressif ESP32 license.
// It returns the number of entries found (files only).
//
unsigned long listDir(fs::FS &fs, const char * dirname, String prefix) {
  Serial.print(prefix);
  Serial.println(dirname);
  unsigned long count = 0;

  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return 0;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return 0;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      count += listDir(fs, file.name(), prefix + " ");
    } else {
      Serial.print(prefix + " ");
      Serial.println(file.name());
      count ++;
    }
    file = root.openNextFile();
  }
  root.close();
  return count;
}

void wipeCache() {
  caching = false;

  if (!SPIFFS.format()) {
    Serial.println("SPIFFS formatting failed.");
    return;
  }

  Serial.println("Formatted.");
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS mount after formatting failed.");
    return;
  };

  for (int i = 0; i < 255; i++)
    SPIFFS.mkdir(String(i));

  Serial.println("Directory structure created.");
  listDir(SPIFFS, "/", "");
  caching = true;
};

String uid2path(MFRC522::Uid uid) {
  String path = "/uid-";
  for (int i = 0; i < uid.size; i++) {
    path += String(uid.uidByte[i], DEC);
    if (i == 0)
      path += "/";
    else
      path += ".";
  };
  return path;
}

bool checkCache(MFRC522::Uid uid) {
  String path = uid2path(uid) + ".lastOK";
  return SPIFFS.exists(path);
}

void setCache(MFRC522::Uid uid, bool ok) {
  String path = uid2path(uid) + ".lastOK";
  if (ok) {
    File f = SPIFFS.open(path, "w");
    f.println(cnt_minutes);
    f.close();
  } else {
    SPIFFS.remove(path);
  }
}


void closeDoor() {
  doorstate = CLOSED;
  digitalWrite(SOLENOID_GPIO, SOLENOID_OFF);

}

void openDoor() {
  char msg[255];
  snprintf(msg, sizeof(msg), "[%s] Engaging solenoid", pname);
  doorstate = OPEN;
  client.publish(log_topic, msg);
  Serial.println(msg);

  // Engage solenoid.
  digitalWrite(SOLENOID_GPIO, SOLENOID_ENGAGED);

  // according to Ticker examples - this will reset/overwrite
  // any already running tickers.
  //
  solenoid.once_ms(DOOR_OPEN_DELAY, &closeDoor);
};

bool isOpen() {
  return digitalRead(SOLENOID_GPIO) == SOLENOID_ENGAGED;
}

bool isClosed() {
  return !isOpen();
}

MFRC522::Uid uid;

String uidToString(MFRC522::Uid uid) {
  String uidStr = "";
  for (int i = 0; i < uid.size; i++) {
    if (i) uidStr += "-";
    uidStr += String(uid.uidByte[i], DEC);
  };
  return uidStr;
}

void callback(char* topic, byte * payload, unsigned int length) {
  char buff[256];

  if (strcmp(topic, door_topic)) {
    Serial.printf("Received an unexepcted %d byte message on topic <%s>, ignoring.", length, topic);
    // We intentinally do not log this message to a MQTT channel-as to reduce the
    // risk of (amplification) loops due to a misconfiguration. We do increase the counter
    // so indirectly this does show up in the MQTT log.
    //
    cnt_mqttfails ++;
    return;
  };

  int l = 0;
  for (int i = 0; l < sizeof(buff) - 1 && i < length; i++) {
    char c = payload[i];
    if (c >= 32 && c < 128)
      buff[l++] = c;
  };

  buff[l] = 0;
  Serial.println("Handling command: <" + String(buff) + ">");

  if (!strcmp(buff, "reboot")) {
    ESP.restart();
    return;
  }

  if (!strcmp(buff, "report")) {
    reportStats();
    return;
  }

  if (!strcmp(buff, "cache")) {
    char msg[255];
    unsigned long count = listDir(SPIFFS, "/", "");

    snprintf(msg, sizeof(msg), "[%s] Number of files in the cache: %lu", pname, count);
    client.publish(log_topic, msg);
    Serial.println(msg);
    return;
  }

  if (!strcmp(buff, "purge")) {
    char msg[255];
    wipeCache();

    snprintf(msg, sizeof(msg), "[%s] Purged cache.", pname);
    client.publish(log_topic, msg);
    Serial.println(msg);

    return;
  }

  if (!strncmp(buff, "purge ", 6)) {
    char msg[255];
    char * ptag = buff + 6;
    char * p = ptag;
    for (uid.size = 0; uid.size < sizeof(uid.uidByte) && *p;) {
      while (*p && !isdigit(*p))
        p++;

      errno = 0;
      byte b = (byte) strtol(p, NULL, 10);
      if (b == 0 && (errno || *p != '0')) // it seems ESP32 does not set errno ?!
        break;
      uid.uidByte[uid.size++] = b;

      while (*p && isdigit(*p))
        p++;
    }
    if (uid.size) {
      snprintf(msg, sizeof(msg), "[%s] Purged cache of UID <%s> (payload %s)", pname, uidToString(uid).c_str(), ptag);
      setCache(uid, false);
    } else {
      snprintf(msg, sizeof(msg), "[%s] Ignored purge request for uid-payload <%s> ", pname, ptag);
      cnt_mqttfails ++;
    }
    client.publish(log_topic, msg);
    Serial.println(msg);

    return;
  }

  if (!strcmp(buff, "open")) {
    Serial.println("Opening door.");
    openDoor();
    // So we have a lovely security hole here - if someone swipes a card
    // that has access; and then quickly (before the server respons) swipes
    // a card that is unknown -- that other card gets caches. And as the system
    // does not see a 'deny' in the 'as-is' situation - we have a nice window
    // of naughtyness.
    //
    if (caching && uid.size)
      setCache(uid, true);
    uid.size = 0;
    return;
  };

  if (caching)
    setCache(uid, false);

  uid.size = 0;

  char msg[256];
  snprintf(msg, sizeof(msg), "[%s] Cannot parse reply <%s> [len=%d, payload len=%d] or denied access.",
           pname, buff, l, length);

  client.publish(log_topic, msg);
  Serial.println(msg);

  cnt_mqttfails ++;
}

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif
double coreTemp() {
  double   temp_farenheit = temprature_sens_read();
  return ( temp_farenheit - 32 ) / 1.8;
}

void reportStats() {
  char buff[255];
  snprintf(buff, sizeof(buff),
           "[%s] alive-uptime %02ld:%02ld, "
           "mqtt %s (state %d) "
           "swipes %ld, opens %ld, closes %ld, fails %ld, mis-swipes %ld, mqtt reconnects %ld, mqtt fails %ld, "
           "door %s, state %s(%d), caching %s, temperature %.1f, failed rfid-selftests: %ld",
           pname, cnt_minutes / 60, (cnt_minutes % 60),
           client.connected() ? "connected" : "not-connected", client.state(),
           cnt_cards,
           cnt_opens, cnt_closes, cnt_fails, cnt_misreads, cnt_reconnects, cnt_mqttfails,
           isOpen() ? "open" : "closed",
           doorstates_names[doorstate], doorstate,
           caching ? "on" : "off",
           coreTemp(),
           cnt_failed_rfid
          );
  client.publish(log_topic, buff);
  Serial.println(buff);
}


void loop()
{
#ifdef AARTLED_GPIO
  // Slow on/off 1 second pattern if we are alive. Very fast flash if we've lost a
  // network connectionm and a quickish flash while we think the door is open.
  //
  {
    static unsigned long aart = 0;
    unsigned long del = 1000;
    if (!eth_connected or !client.connected())
      del = 100;
    else if (doorstate != CLOSED)
      del = 300;

    if (millis() - aart > del) {
      digitalWrite(AARTLED_GPIO, !digitalRead(AARTLED_GPIO));
      aart = millis();
    }
  }
#endif
  if (eth_connected) {
    if (ota)
      ArduinoOTA.handle();
    else
      enableOTA();

    client.loop();
    if (!client.connected()) {
      long now = millis();
      if (now - lastReconnectAttempt > 10 * 1000) {
        lastReconnectAttempt = now;
        reconnect();
      }
    }
  } else {
    if (client.connected())
      client.disconnect();
  }

  static doorstate_t lastdoorstate = CLOSED;
  if (lastdoorstate != doorstate) {
    char msg[255];
    snprintf(msg, sizeof(msg), "[%s] Change from state %s(%d) to %s(%d)",
             pname,
             doorstates_names[lastdoorstate], lastdoorstate, doorstates_names[doorstate], doorstate);
    Serial.println(msg);
#ifndef LOCALMQTT
    client.publish(log_topic, msg);
#endif
    lastdoorstate = doorstate;
    last_doorstatechange = millis();
  }

  switch (doorstate) {
    case CHECKINGCARD:
      if (millis() - last_doorstatechange > CACHE_FALLBACK_TIMEOUT && caching && checkCache(uid)) {
        char msg[255];
        snprintf(msg, sizeof(msg), "[%s] Allowing door to open based on cached information.", pname);
        Serial.println(msg);
        client.publish(log_topic, msg);
        openDoor();
      }
      if (millis() - last_doorstatechange > MAX_RESPONSE_TIMEOUT) {
        char msg[255];
        snprintf(msg, sizeof(msg), "[%s] No reply from server. Keeping door closed.", pname);
        Serial.println(msg);
        client.publish(log_topic, msg);
        closeDoor();
      }
      break;
    case OPEN:
      break;
    case CLOSED:
#ifdef WIRECHECK_GPIO
      {
	static unsigned long lastWireFail = 0;
        if (digitalRead(WIRECHECK_GPIO) == WIRECHECK_REDLED) {
	   lastWireFail = millis();
	} else {
           if (millis() - lastWireFail > 2 * DOOR_OPEN_DELAY && lastWireFail != 0) {
		lastWireFail += 300 * 1000; // 5 minute reporting interval.
        
                 char msg[255];
                 snprintf(msg, sizeof(msg), "[%s] RED led still on - despite closed door. Possible wire issue.", pname);
                 Serial.println(msg);
                 client.publish(log_topic, msg);
           } 
        }
      }
#endif
      break;
    default:
      break;
  };

#ifdef WIRECHECK_GPIO
  {
    static unsigned long last = 0;
    if (millis() - last > 5000) {
      char msg[256];
      snprintf(msg,sizeof(msg),"%s: Wire pin: %d", pname, digitalRead(WIRECHECK_GPIO));
                 Serial.println(msg);
                 client.publish(log_topic, msg);      
    }
  }
#endif

  // catch any logic errors or strange cases where the door is open while we think
  // we are not doing anything.
  //
  if ((millis() - last_doorstatechange > 10 * DOOR_OPEN_DELAY) && ((doorstate != CLOSED) || (!isClosed()))) {
    closeDoor();
    static unsigned long lastReport = 0;
    if (millis() - lastReport > 3000 || doorstate != lastdoorstate) {
      lastReport = millis();
      char msg[256];
      snprintf(msg, sizeof(msg), "[%s] Door %sin an odd state-forcing close.", pname, doorstate == lastdoorstate ? "still " : "");
      Serial.println(msg);
      client.publish(log_topic, msg);
    };
    cnt_fails ++;
  }

  {
    static unsigned long tock = 0;
    if (millis() - tock > REPORTING_PERIOD) {
      static unsigned long last_min = 0;
      unsigned long delta = millis() - last_min;
      unsigned int mins = delta / 60 / 1000;

      mfrc522.PCD_Reset();
      if (!mfrc522.PCD_PerformSelfTest()) {
        cnt_failed_rfid++;

        char msg[256];
        snprintf(msg, sizeof(msg), "[%s] RFID selftest failed (%d times since boot%s).", 
		pname, cnt_failed_rfid, 
		(cnt_failed_rfid == 3) ? " and will reboot on next fail." : "");
        Serial.println(msg);
        client.publish(log_topic, msg);

        if (cnt_failed_rfid > 3)
          ESP.restart();
      };
      mfrc522.PCD_Reset();
      reinitRFID();
      
      if (mins > 0) {
        cnt_minutes += mins;
        last_min += mins * 60 * 1000;
      }
      reportStats();
      tock = millis();
    }
  }

  if (irqCardSeen) {
    if (mfrc522.PICC_ReadCardSerial()) {
      uid = mfrc522.uid;
      String uidStr = uidToString(uid);

      String pyStr = "[";
      for (int i = 0; i < uid.size; i++) {
        if (i) pyStr += ", ";
        pyStr += String(uid.uidByte[i], DEC);
      };
      pyStr += "]";

      cnt_cards++;
      client.publish(rfid_topic, pyStr.c_str());

      char msg[256];
#ifndef LOCALMQTT
      snprintf(msg, sizeof(msg), "[%s] Tag <%s> (len=%d) swiped", pname, uidStr.c_str(), uid.size);
#else
      snprintf(msg, sizeof(msg), "[%s] Tag swiped", pname);
#endif
      client.publish(log_topic, msg);
      Serial.println(msg);

      doorstate = CHECKINGCARD;
    } else {
      Serial.println("Misread.");
      cnt_misreads++;
    }
    mfrc522.PICC_HaltA(); // Stop reading

    clearInt(mfrc522);
    irqCardSeen = false;
  };

  // Re-arm/retrigger the scanning regularly.
  {
    static unsigned long reminderToRead = 0;
    if (millis() - reminderToRead > 100) {
      activateRec(mfrc522);
      reminderToRead = millis();
    }
  }
}
