// SSDPBoth June 2019

#include <ESP8266WebServer.h>                  // for the webserver
#include <DNSServer.h>                         // DNS Server for Access Point Wise
#include "SSDPBoth.h"                          // Windows discovering devices

#define SKETCH_VERSION "1.0.0"                 // Current version
#define HOSTNAME       "SSDPBoth"              // Current hostname
#define DNS_PORT       53                      // Capture DNS requests port
#define WEB_PORT       80                      // HTTP and SSDP port

#define STA_SSID       "Your-SSID"
#define STA_PWD        "Your-Password"

DNSServer              dnsServer;              // Create the DNS object
ESP8266WebServer       webServer(WEB_PORT);    // HTTP server

void SSDPconfig(char* SSDPName) {
  char chaine[25];
  SSDP.setName(SSDPName);
  SSDP.setManufacturer(HOSTNAME);
  SSDP.setManufacturerURL("https://github.com/BRIOTPatrick/SSDPBoth");
  sprintf(chaine, "ESP8266-01 - %dKb", ESP.getFlashChipRealSize() / 1024);
  SSDP.setModelName(chaine);
  sprintf(chaine, "http://%s.local", SSDPName);
  SSDP.setModelURL(chaine);
  sprintf(chaine, "%06X", ESP.getFlashChipId());
  SSDP.setModelNumber(chaine);
  SSDP.setURL("/");
  SSDP.setSerialNumber(HOSTNAME" "SKETCH_VERSION);
  SSDP.begin();
}

void handle_SSDP(void) {
  static const char responseHTML[] PROGMEM =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"manageport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>SSDPBoth "SKETCH_VERSION"</title>\n"
    "</head>\n"
    "<body>\n"
    "<br>\n"
    "<p>SSDPBoth on STA and AP Demo.</p>\n"
    "<br><br>\n"
    "<a href=\"/manageSTA\"><button>%s</button></a> SSDP in STA network is currently '%s'<br>\n"
    "<a href=\"/manageAP\" ><button>%s</button></a> SSDP in AP network is currently '%s'<br>\n"
     "</body>\n"
    "</html>\n";

  int bufsize;
  char *buffer;
  SN(buffer, bufsize,
     responseHTML,
     SSDP.manage(STA_READ) ? "SHOW"    : "HIDE",
     SSDP.manage(STA_READ) ? "hidden" : "visible",
     SSDP.manage(AP_READ)  ? "SHOW"    : "HIDE",
     SSDP.manage(AP_READ)  ? "hidden" : "visible"
    );
  webServer.send(200, "text/html", buffer);
  free(buffer);
}

void handle_manage(bool STA_AP){
  STA_AP ? SSDP.manage(STA_TOGGLE) : SSDP.manage(AP_TOGGLE);
  webServer.sendHeader("Location", "/",true);   //Redirect to root html web page  
  webServer.send(302, "text/plane", "");
}

void setup() {
  DEBUG_BEGIN(115200);
  DEBUG_PRINTF(HOSTNAME" starting...\n");

  WiFi.mode(WIFI_AP);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(STA_SSID, STA_PWD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    yield();
  }
  DEBUG_PRINT(WiFi.localIP());
  DEBUG_PRINTF(" connected.\n");

  WiFi.softAP(HOSTNAME);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  webServer.on("/description.xml", HTTP_GET, []() { SSDP.schema(webServer.client()); });
  webServer.on("/manageSTA", []() { handle_manage(true);  });
  webServer.on("/manageAP" , []() { handle_manage(false); });
  webServer.onNotFound(handle_SSDP);
  webServer.begin();
  SSDPconfig(HOSTNAME);
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  SSDP.update();
}
