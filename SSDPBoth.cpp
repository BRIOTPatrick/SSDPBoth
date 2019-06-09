/*
ESP8266 Simple Service Discovery STA and AP
Copyright (c) Patrick Briot, May 29th, 2019.
 
Original (ESP8266) version by Hristo Gochkov, 2015.

Original (Arduino) version by Filippo Sallemi, July 23, 2014.
Can be found at: https://github.com/nomadnt/uSSDP

License (MIT license):
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include "SSDPBoth.h"
#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "include/UdpContext.h"

#define SSDP_INTERVAL       1200
#define SSDP_PORT           1900
#define SSDP_METHOD_SIZE    10
#define SSDP_URI_SIZE       2
#define SSDP_BUFFER_SIZE    64
#define SSDP_MULTICAST_TTL  2

static const ip_addr_t mcast=IPADDR4_INIT_BYTES(239, 255, 255, 250);

static const char _ssdp_response_template[] PROGMEM =
  "HTTP/1.1 200 OK\r\n"
  "EXT:\r\n";

static const char _ssdp_notify_template[] PROGMEM =
  "NOTIFY * HTTP/1.1\r\n"
  "HOST: 239.255.255.250:1900\r\n"
  "NTS: ssdp:alive\r\n";

static const char _ssdp_packet_template[] PROGMEM =
  "%s"                                     // _ssdp_response_template or _ssdp_notify_template
  "CACHE-CONTROL: max-age=%u\r\n"          // SSDP_INTERVAL
  "SERVER: ESP8266/1.0 UPNP/1.1 %s/%s\r\n" // _modelName / _modelNumber
  "USN: uuid:%s\r\n"                       // _uuid
  "%s: %s\r\n"                             // "NT" or "ST" : _deviceType
  "LOCATION: http://"IPSTR":%u/%s\r\n"     // _sta or _ap ip : _port / _schemaURL
  "\r\n";

static const char _ssdp_schema_template[] PROGMEM =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: text/xml\r\n"
  "Connection: close\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "\r\n"
  "<?xml version=\"1.0\"?>"
  "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
  "<specVersion>"
  "<major>1</major>"
  "<minor>0</minor>"
  "</specVersion>"
  "<URLBase>http://"IPSTR":%u/</URLBase>"  // _sta or _ap ip : _port
  "<device>"
  "<deviceType>%s</deviceType>"
  "<friendlyName>%s</friendlyName>"
  "<presentationURL>%s</presentationURL>"
  "<serialNumber>%s</serialNumber>"
  "<modelName>%s</modelName>"
  "<modelNumber>%s</modelNumber>"
  "<modelURL>%s</modelURL>"
  "<manufacturer>%s</manufacturer>"
  "<manufacturerURL>%s</manufacturerURL>"
  "<UDN>uuid:%s</UDN>"
  "</device>"
  "</root>\r\n"
  "\r\n";

//*****************************************************************************************************************************
// Public functions
//*****************************************************************************************************************************
SSDPBoth::SSDPBoth()
: _pending(false)
, _ttl(SSDP_MULTICAST_TTL)
, _port(80)
, _delay(0)
, _millis(millis())
, _notify_time(0)
, _process_time(0)
, _respondToPort(0)
, _hideSTA(false)
, _hideAP(false)
{
  end();
  _server = new UdpContext;
  _server->ref();

  _friendlyName[0] = '\0';
  _manufacturer[0] = '\0';
  _manufacturerURL[0] = '\0';
  _modelName[0] = '\0';
  _modelNumber[0] = '\0';
  _modelURL[0] = '\0';
  _presentationURL[0] = '\0';
  _serialNumber[0] = '\0';
  // "urn:schemas-upnp-org:device:Basic:1" should work but in fact, not !
  sprintf(_deviceType, "upnp:rootdevice");
  sprintf(_schemaURL, "description.xml");
  sprintf(_uuid, "38323636-4558-4dda-9188-cda0e6%06x", ESP.getChipId());
}

SSDPBoth::~SSDPBoth() {
  end();
}

bool SSDPBoth::begin() {
  wifi_get_ip_info(STATION_IF, &_sta);
  wifi_get_ip_info(SOFTAP_IF, &_ap);
  DEBUG_PRINTF("SSDP STA IP:"IPSTR" AP IP:"IPSTR" MULTICAST:"IPSTR"\n", IP2STR(&_sta.ip), IP2STR(&_ap.ip), IP2STR(&mcast));

  if ((_sta.ip.addr) && !_hideSTA && (igmp_joingroup(&_sta.ip, &mcast) != ERR_OK )) {
    DEBUG_PRINTF("SSDP : STA failed to join igmp group\n");
    return false;
  }
  if ((_ap.ip.addr) && !_hideAP && (igmp_joingroup(&_ap.ip, &mcast) != ERR_OK )) {
    DEBUG_PRINTF("SSDP : AP failed to join igmp group\n");
    return false;
  }

  if (!_server->listen(*IP_ADDR_ANY, SSDP_PORT)) {
    DEBUG_PRINTF("SSDP : listen on network failed\n");
    return false;
  }

  _server->setMulticastInterface(_sta.ip.addr);
  _server->setMulticastTTL(_ttl);
  _server->onRx(std::bind(&SSDPBoth::_update, this));
  if (!_server->connect(mcast, SSDP_PORT)) {
    DEBUG_PRINTF("SSDP : connect to server failed\n");
    return false;
  }

  return true;
}

void SSDPBoth::end() {
  if (_server) {
    _server->disconnect();
    if ((_ap.ip.addr) && (igmp_leavegroup(&_ap.ip, &mcast) != ERR_OK )) {
      DEBUG_PRINTF("SSDP AP failed to leave igmp group\n");
    }
    if ((_sta.ip.addr) && (igmp_leavegroup(&_sta.ip, &mcast) != ERR_OK )) {
      DEBUG_PRINTF("SSDP STA failed to leave igmp group\n");
    }
    _server->unref();
    _server = 0;
  }
}

void SSDPBoth::update() {
  if (millis() - _millis > SSDP_DELTA_MS) {
    _millis = millis();
    _update();
  }
}

void SSDPBoth::schema(WiFiClient client) {
  uint32_t localIP = client.localIP();
  if ((_sta.ip.addr && !_hideSTA && (_sta.ip.addr == localIP)) ||
      (_ap.ip.addr  && !_hideAP  && (_ap.ip.addr  == localIP))) {
    client.printf(_ssdp_schema_template,
                  IP2STR((const ip_addr_t*)&localIP),
                  _port,
                  _deviceType,
                  _friendlyName,
                  _presentationURL,
                  _serialNumber,
                  _modelName,
                  _modelNumber,
                  _modelURL,
                  _manufacturer,
                  _manufacturerURL,
                  _uuid
                 );
  }
}

int SSDPBoth::manage(ssdp_manage_t manage) {
  if (manage == STA_READ) return _hideSTA;
  if (manage == AP_READ)  return _hideAP;
  if (!_hideAP) {
    if (manage == STA_TOGGLE) _hideSTA = 1-_hideSTA;
    if (manage == STA_SHOW)   _hideSTA = 0;
    if (manage == STA_HIDE)   _hideSTA = 1;
  }
  if (!_hideSTA) {
    if (manage == AP_TOGGLE) _hideAP = !_hideAP;
    if (manage == AP_SHOW)   _hideAP = 0;
    if (manage == AP_HIDE)   _hideAP = 1;
  }
  return -1;
}

void SSDPBoth::setParam(ssdp_setparams_t param, const char *paramchar, ...) {
  va_list args;
  va_start(args, paramchar);
  switch (param) {
  case SET_DEVICETYPE:        vsnprintf(_deviceType, sizeof(_deviceType)-1, paramchar, args); break;
  case SET_UUID:              vsnprintf(_uuid, sizeof(_uuid)-1, paramchar, args); break;
  case SET_NAME:              vsnprintf(_friendlyName, sizeof(_friendlyName)-1, paramchar, args); break;
  case SET_URL:               vsnprintf(_presentationURL, sizeof(_presentationURL)-1, paramchar, args); break;
  case SET_SCHEMAURL:         vsnprintf(_schemaURL, sizeof(_schemaURL)-1, paramchar, args); break;
  case SET_SERIALNUMBER:      vsnprintf(_serialNumber, sizeof(_serialNumber)-1, paramchar, args); break;
  case SET_MODELNAME:         vsnprintf(_modelName, sizeof(_modelName)-1, paramchar, args); break;
  case SET_MODELNUMBER:       vsnprintf(_modelNumber, sizeof(_modelNumber)-1, paramchar, args); break;
  case SET_MODELURL:          vsnprintf(_modelURL, sizeof(_modelURL)-1, paramchar, args); break;
  case SET_MANUFACTURER:      vsnprintf(_manufacturer, sizeof(_manufacturer)-1, paramchar, args); break;
  case SET_MANUFACTURERURL:   vsnprintf(_manufacturerURL, sizeof(_manufacturerURL)-1, paramchar, args); break;
  }
  va_end(args);
}

void SSDPBoth::setSerialNumber(const uint32_t serialNumber) { snprintf(_serialNumber, sizeof(uint32_t) * 2 + 1, "%08X", serialNumber); }
void SSDPBoth::setHTTPPort(uint16_t port)                   { _port = port; }
void SSDPBoth::setTTL(const uint8_t ttl)                    { _ttl = ttl; }

//*****************************************************************************************************************************
// Private functions
//*****************************************************************************************************************************
void SSDPBoth::_send(ssdp_method_t method) {
  char *buffer; int bufsize;
  SN(buffer, bufsize,
     _ssdp_packet_template,
     (method == NONE) ? _ssdp_response_template : _ssdp_notify_template,
     SSDP_INTERVAL,
     _modelName,
     _modelNumber,
     _uuid,
     (method == NONE) ? "ST" : "NT",
     _deviceType,
     IP2STR(!(_respondToAddr.addr) || (_sta.ip.addr && ip_addr_netcmp(&_respondToAddr, &_sta.ip, &_sta.netmask)) ? &_sta.ip : &_ap.ip),
     _port,
     _schemaURL
    );
  _server->append(buffer, bufsize);
  DEBUG_PRINTF("SSDP: STA="IPSTR" AP="IPSTR" Respond="IPSTR" buffer=\n%sEnd buffer\n", IP2STR(&_sta.ip), IP2STR(&_ap.ip), IP2STR(&_respondToAddr), buffer);
  free(buffer);

  if (method == NONE) {
    _server->send(&_respondToAddr, _respondToPort);
    DEBUG_PRINTF("SSDP: Sending Response to "IPSTR":%d\n", IP2STR(&_respondToAddr), _respondToPort);
  } else {
    _server->send(&mcast, SSDP_PORT);
    DEBUG_PRINTF("SSDP: Sending Notify to "IPSTR":%d\n", IP2STR(&mcast), SSDP_PORT);
  }
}

void SSDPBoth::_update() {
  if (!_pending && _server->next()) {
    ssdp_method_t method = NONE;

    _respondToAddr.addr = _server->getRemoteAddress();
    _respondToPort = _server->getRemotePort();

    typedef enum {METHOD, URI, PROTO, KEY, VALUE, ABORT} states;
    states state = METHOD;

    typedef enum {START, MAN, ST, MX} headers;
    headers header = START;

    uint8_t cursor = 0;
    uint8_t cr = 0;

    char buffer[SSDP_BUFFER_SIZE] = {0};

    while (_server->getSize() > 0) {
      char c = _server->read();

      (c == '\r' || c == '\n') ? cr++ : cr = 0;

      switch (state) {
        case METHOD:
          if (c == ' ') {
            if (strcmp(buffer, "M-SEARCH") == 0) method = SEARCH;

            state = (method == NONE) ? ABORT : URI;
            cursor = 0;

          } else if (cursor < SSDP_METHOD_SIZE - 1) {
            buffer[cursor++] = c;
            buffer[cursor] = '\0';
          }
          break;
        case URI:
          if (c == ' ') {
            state = (strcmp(buffer, "*")) ? ABORT : PROTO;
            cursor = 0;
          } else if (cursor < SSDP_URI_SIZE - 1) {
            buffer[cursor++] = c;
            buffer[cursor] = '\0';
          }
          break;
        case PROTO:
          if (cr == 2) {
            state = KEY;
            cursor = 0;
          }
          break;
        case KEY:
          if (cr == 4) {
            _pending = true;
            _process_time = millis();
          }
          else if (c == ' ') {
            cursor = 0;
            state = VALUE;
          }
          else if (c != '\r' && c != '\n' && c != ':' && cursor < SSDP_BUFFER_SIZE - 1) {
            buffer[cursor++] = c;
            buffer[cursor] = '\0';
          }
          break;
        case VALUE:
          if (cr == 2) {
            switch (header) {
              case START:
                break;
              case MAN:
                DEBUG_PRINTF("SSDP MAN: %s\n", (char *)buffer);
                break;
              case ST:
                if (strcmp(buffer, "ssdp:all")) {
                  state = ABORT;
                  DEBUG_PRINTF("SSDP REJECT: %s\n", (char *)buffer);
                }
                // if the search type matches our type, we should respond instead of ABORT
                if (strcasecmp(buffer, _deviceType) == 0) {
                  _pending = true;
                  _process_time = millis();
                  state = KEY;
                }
                break;
              case MX:
                _delay = random(0, atoi(buffer)) * 1000L;
                break;
            }

            if (state != ABORT) {
              state = KEY;
              header = START;
              cursor = 0;
            }
          } else if (c != '\r' && c != '\n') {
            if (header == START) {
              if (strncmp(buffer, "MA", 2)  == 0) header = MAN;
              else if (strcmp(buffer, "ST") == 0) header = ST;
              else if (strcmp(buffer, "MX") == 0) header = MX;
            }

            if (cursor < SSDP_BUFFER_SIZE - 1) {
              buffer[cursor++] = c;
              buffer[cursor] = '\0';
            }
          }
          break;
        case ABORT:
          _pending = false; _delay = 0;
          break;
      }
    }
  }

  if (_pending && (millis() - _process_time) > _delay) {
    _pending = false; _delay = 0;
    _send(NONE);
  } else if(_notify_time == 0 || (millis() - _notify_time) > (SSDP_INTERVAL * 1000L)){
    _notify_time = millis();
    _send(NOTIFY);
  }

  if (_pending) {
    while (_server->next())
      _server->flush();
  }
}

#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_SSDP)
SSDPBoth SSDP;
#endif
