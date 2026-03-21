/*
 * nfc_spotify_sonos.ino
 *
 * Scans an NFC tag, reads its NDEF URL, parses it as a Spotify link,
 * and plays the track / album / artist on a Sonos speaker via UPnP/SOAP.
 *
 * Supported tag types : NTAG213 / NTAG215 / NTAG216, MIFARE Classic
 * Supported Spotify   : track, album, artist (top tracks)
 *
 * Wiring (VSPI):
 *   RC522 SDA  → GPIO 5
 *   RC522 SCK  → GPIO 18
 *   RC522 MOSI → GPIO 23
 *   RC522 MISO → GPIO 19
 *   RC522 RST  → GPIO 22
 *   RC522 3.3V → 3.3V  (NOT 5V)
 *   RC522 GND  → GND
 *
 * Dependencies (Library Manager):
 *   MFRC522 by GithubCommunity
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

// How long (ms) to ignore the same UID after a successful read.
// Prevents the same tag triggering playback repeatedly while held near the reader.
const unsigned long DEBOUNCE_MS = 5000;

// ── RC522 pin definitions ─────────────────────────────────────────────────────

#define SS_PIN   5
#define RST_PIN  22
#define SCK_PIN  18
#define MISO_PIN 19
#define MOSI_PIN 23

// ── Constants ─────────────────────────────────────────────────────────────────

#define MAX_NDEF_BYTES 512

MFRC522 mfrc522(SS_PIN, RST_PIN);

// ── Last-seen UID (for debounce) ──────────────────────────────────────────────

static uint8_t       lastUID[10];
static uint8_t       lastUIDLen    = 0;
static unsigned long lastReadTime  = 0;

// ── URI prefix table ──────────────────────────────────────────────────────────

const char* URI_PREFIXES[] = {
  "",                           // 0x00
  "http://www.",                // 0x01
  "https://www.",               // 0x02
  "http://",                    // 0x03
  "https://",                   // 0x04
  "tel:",                       // 0x05
  "mailto:",                    // 0x06
  "ftp://anonymous:anonymous@", // 0x07
  "ftp://ftp.",                 // 0x08
  "ftps://",                    // 0x09
  "sftp://",                    // 0x0A
  "smb://",                     // 0x0B
  "nfs://",                     // 0x0C
  "ftp://",                     // 0x0D
  "dav://",                     // 0x0E
  "news:",                      // 0x0F
  "telnet://",                  // 0x10
  "imap:",                      // 0x11
  "rtsp://",                    // 0x12
  "urn:",                       // 0x13
  "pop:",                       // 0x14
  "sip:",                       // 0x15
  "sips:",                      // 0x16
  "tftp:",                      // 0x17
  "btspp://",                   // 0x18
  "btl2cap://",                 // 0x19
  "btgoep://",                  // 0x1A
  "tcpobex://",                 // 0x1B
  "irdaobex://",                // 0x1C
  "file://",                    // 0x1D
  "urn:epc:id:",                // 0x1E
  "urn:epc:tag:",               // 0x1F
  "urn:epc:pat:",               // 0x20
  "urn:epc:raw:",               // 0x21
  "urn:epc:",                   // 0x22
  "urn:nfc:",                   // 0x23
};
#define URI_PREFIX_COUNT (sizeof(URI_PREFIXES) / sizeof(URI_PREFIXES[0]))

// =============================================================================
// DATA TYPES
// =============================================================================

struct NDEFResult {
  bool    success;
  char    url[256];
  char    rawType[16];
  uint8_t tnf;
  char    errorMsg[64];
};

struct SpotifyItem {
  String type;   // "track", "album", "artist"
  String id;
  bool   valid;
};

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

// NFC
bool       readNDEFFromUltralight(uint8_t* buf, uint16_t* len);
bool       readNDEFFromClassic(uint8_t* buf, uint16_t* len);
bool       parseTLV(const uint8_t* buf, uint16_t bufLen, uint8_t* ndefBuf, uint16_t* ndefLen);
NDEFResult parseNDEF(const uint8_t* ndef, uint16_t ndefLen);
bool       isUltralightFamily();
bool       isClassicFamily();
bool       isSameUID(const uint8_t* uid, uint8_t uidLen);
void       saveUID(const uint8_t* uid, uint8_t uidLen);
void       printUID();

// Spotify / Sonos
SpotifyItem parseSpotifyInput(String input);
void        playSpotifyItem(SpotifyItem item);
void        playTrack(String id);
void        playAlbum(String id);
void        playArtist(String id);
String      albumMetadata(String id);
String      artistMetadata(String id);
bool        soapRequest(String action, String body);
bool        sonosPlay();
bool        sonosSetURI(String uri, String metadata);
bool        sonosAddURIToQueue(String uri, String metadata);
bool        sonosSeekToQueue();
bool        sonosSeekTrack(int trackNum);

// =============================================================================
// SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial);
  delay(500);

  Serial.println("\n=============================");
  Serial.println("  NFC → Spotify → Sonos");
  Serial.println("=============================");

  // ── WiFi ──────────────────────────────────────────────────────────────────
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > 15000) {
      Serial.println("\n❌ WiFi timeout. Check SSID/password. Halting.");
      while (true) delay(1000);
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // ── RC522 ──────────────────────────────────────────────────────────────────
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  delay(100);

  byte version = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("❌ RC522 not detected. Check wiring and 3.3V power. Halting.");
    while (true) delay(1000);
  }
  Serial.printf("✅ RC522 firmware 0x%02X ready.\n", version);
  Serial.println("\nHold an NFC tag near the reader...\n");
}

// =============================================================================
// MAIN LOOP
// =============================================================================

void loop() {
  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi lost — reconnecting...");
    WiFi.reconnect();
    delay(3000);
    return;
  }

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial())   return;

  // ── Debounce: ignore the same tag within DEBOUNCE_MS ─────────────────────
  unsigned long now = millis();
  if (isSameUID(mfrc522.uid.uidByte, mfrc522.uid.size) &&
      (now - lastReadTime) < DEBOUNCE_MS) {
    mfrc522.PICC_HaltA();
    return;
  }
  saveUID(mfrc522.uid.uidByte, mfrc522.uid.size);
  lastReadTime = now;

  // ── Identify tag ──────────────────────────────────────────────────────────
  Serial.println("─────────────────────────────");
  printUID();
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.printf("Tag type : %s\n", mfrc522.PICC_GetTypeName(piccType));

  // ── Read raw memory ───────────────────────────────────────────────────────
  uint8_t  rawBuf[MAX_NDEF_BYTES];
  uint8_t  ndefBuf[MAX_NDEF_BYTES];
  uint16_t rawLen  = 0;
  uint16_t ndefLen = 0;
  bool     readOk  = false;

  if      (isUltralightFamily()) readOk = readNDEFFromUltralight(rawBuf, &rawLen);
  else if (isClassicFamily())    readOk = readNDEFFromClassic(rawBuf, &rawLen);
  else {
    Serial.println("⚠️  Unsupported tag type.");
    goto cleanup;
  }

  if (!readOk) {
    Serial.println("❌ Failed to read tag memory.");
    goto cleanup;
  }

  // ── Unwrap TLV ────────────────────────────────────────────────────────────
  if (!parseTLV(rawBuf, rawLen, ndefBuf, &ndefLen)) {
    Serial.println("❌ No NDEF TLV found — tag may be empty or unformatted.");
    goto cleanup;
  }

  {
    // ── Parse NDEF ──────────────────────────────────────────────────────────
    NDEFResult ndef = parseNDEF(ndefBuf, ndefLen);

    if (!ndef.success) {
      Serial.printf("⚠️  NDEF parse issue: %s\n", ndef.errorMsg);
      goto cleanup;
    }

    Serial.printf("URL: %s\n", ndef.url);

    // ── Parse as Spotify URL ────────────────────────────────────────────────
    SpotifyItem item = parseSpotifyInput(String(ndef.url));

    if (!item.valid) {
      Serial.println("⚠️  URL is not a recognised Spotify track/album/artist link.");
      goto cleanup;
    }

    Serial.printf("Spotify %s → %s\n", item.type.c_str(), item.id.c_str());

    // ── Play on Sonos ───────────────────────────────────────────────────────
    playSpotifyItem(item);
  }

cleanup:
  Serial.println();
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// =============================================================================
// NFC — TAG READING
// =============================================================================

bool readNDEFFromUltralight(uint8_t* buf, uint16_t* len) {
  *len = 0;
  const uint8_t START_PAGE = 4;
  const uint8_t MAX_PAGES  = 120;

  for (uint8_t page = START_PAGE; page < START_PAGE + MAX_PAGES; page++) {
    uint8_t pageData[18];
    byte    pageLen = sizeof(pageData);

    if (mfrc522.MIFARE_Read(page, pageData, &pageLen) != MFRC522::STATUS_OK)
      break;

    for (uint8_t b = 0; b < 4 && *len < MAX_NDEF_BYTES; b++)
      buf[(*len)++] = pageData[b];

    // Stop early at NDEF terminator
    for (uint8_t b = (*len >= 4) ? *len - 4 : 0; b < *len; b++)
      if (buf[b] == 0xFE) return true;
  }

  return (*len > 0);
}

bool readNDEFFromClassic(uint8_t* buf, uint16_t* len) {
  *len = 0;

  MFRC522::MIFARE_Key keyA;
  for (byte i = 0; i < 6; i++) keyA.keyByte[i] = 0xFF;

  MFRC522::StatusCode status =
    mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A,
                             4, &keyA, &(mfrc522.uid));

  if (status != MFRC522::STATUS_OK) {
    // Try NFC Forum MAD key
    uint8_t nfcKeyBytes[] = {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7};
    MFRC522::MIFARE_Key nfcKey;
    memcpy(nfcKey.keyByte, nfcKeyBytes, 6);
    status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A,
                                      4, &nfcKey, &(mfrc522.uid));
    if (status != MFRC522::STATUS_OK) {
      Serial.println("   Auth failed — tag may use a custom key.");
      return false;
    }
  }

  for (uint8_t block = 4; block <= 6; block++) {
    uint8_t blockData[18];
    byte    blockLen = sizeof(blockData);
    if (mfrc522.MIFARE_Read(block, blockData, &blockLen) != MFRC522::STATUS_OK)
      return false;
    for (uint8_t b = 0; b < 16 && *len < MAX_NDEF_BYTES; b++)
      buf[(*len)++] = blockData[b];
  }

  return (*len > 0);
}

// =============================================================================
// NFC — TLV / NDEF PARSING
// =============================================================================

bool parseTLV(const uint8_t* buf, uint16_t bufLen,
              uint8_t* ndefBuf, uint16_t* ndefLen) {
  *ndefLen = 0;
  uint16_t i = 0;

  while (i < bufLen) {
    uint8_t tlvType = buf[i++];
    if (tlvType == 0x00) continue;
    if (tlvType == 0xFE) return false;
    if (i >= bufLen)     break;

    uint16_t tlvLen;
    if (buf[i] == 0xFF) {
      i++;
      if (i + 2 > bufLen) return false;
      tlvLen = ((uint16_t)buf[i] << 8) | buf[i + 1];
      i += 2;
    } else {
      tlvLen = buf[i++];
    }

    if (tlvType == 0x03) {
      if (i + tlvLen > bufLen || tlvLen > MAX_NDEF_BYTES) return false;
      memcpy(ndefBuf, buf + i, tlvLen);
      *ndefLen = tlvLen;
      return true;
    }
    i += tlvLen;
  }
  return false;
}

NDEFResult parseNDEF(const uint8_t* ndef, uint16_t ndefLen) {
  NDEFResult result = {};
  result.success = false;

  if (ndefLen < 3) {
    snprintf(result.errorMsg, sizeof(result.errorMsg),
             "NDEF too short (%d bytes)", ndefLen);
    return result;
  }

  uint16_t pos     = 0;
  uint8_t  flags   = ndef[pos++];
  bool     isSR    = (flags & 0x10) != 0;
  bool     hasIL   = (flags & 0x08) != 0;
  result.tnf       = flags & 0x07;
  uint8_t  typeLen = ndef[pos++];

  uint32_t payloadLen;
  if (isSR) {
    payloadLen = ndef[pos++];
  } else {
    if (pos + 4 > ndefLen) {
      snprintf(result.errorMsg, sizeof(result.errorMsg), "Truncated payload length");
      return result;
    }
    payloadLen = ((uint32_t)ndef[pos]   << 24) | ((uint32_t)ndef[pos+1] << 16) |
                 ((uint32_t)ndef[pos+2] <<  8) |  (uint32_t)ndef[pos+3];
    pos += 4;
  }

  uint8_t idLen = 0;
  if (hasIL) idLen = ndef[pos++];

  if (pos + typeLen > ndefLen) {
    snprintf(result.errorMsg, sizeof(result.errorMsg), "Truncated type field");
    return result;
  }
  uint8_t safeLen = min((uint8_t)typeLen, (uint8_t)(sizeof(result.rawType) - 1));
  memcpy(result.rawType, ndef + pos, safeLen);
  result.rawType[safeLen] = '\0';
  pos += typeLen + idLen;

  if (pos + payloadLen > ndefLen) {
    snprintf(result.errorMsg, sizeof(result.errorMsg), "Payload overruns record");
    return result;
  }

  const uint8_t* payload = ndef + pos;

  // URI record (TNF=0x01, type='U')
  if (result.tnf == 0x01 && typeLen == 1 && result.rawType[0] == 'U') {
    if (payloadLen < 1) {
      snprintf(result.errorMsg, sizeof(result.errorMsg), "Empty URI payload");
      return result;
    }
    uint8_t    prefixCode = payload[0];
    const char* prefix    = (prefixCode < URI_PREFIX_COUNT) ? URI_PREFIXES[prefixCode] : "";
    uint32_t   suffixLen  = payloadLen - 1;
    snprintf(result.url, sizeof(result.url), "%s%.*s",
             prefix,
             (int)min(suffixLen, (uint32_t)(sizeof(result.url) - strlen(prefix) - 1)),
             (const char*)(payload + 1));
    result.success = true;

  // Text record (TNF=0x01, type='T') — treat text as potential URL
  } else if (result.tnf == 0x01 && typeLen == 1 && result.rawType[0] == 'T') {
    if (payloadLen < 1) {
      snprintf(result.errorMsg, sizeof(result.errorMsg), "Empty Text payload");
      return result;
    }
    uint8_t  statusByte = payload[0];
    uint8_t  langLen    = statusByte & 0x3F;
    uint32_t textStart  = 1 + langLen;
    if (textStart >= payloadLen) {
      snprintf(result.errorMsg, sizeof(result.errorMsg), "Text record has no content");
      return result;
    }
    uint32_t textLen = payloadLen - textStart;
    snprintf(result.url, sizeof(result.url), "%.*s",
             (int)min(textLen, (uint32_t)(sizeof(result.url) - 1)),
             (const char*)(payload + textStart));
    result.success = true;

  } else {
    snprintf(result.errorMsg, sizeof(result.errorMsg),
             "Unhandled record TNF=0x%02X type='%s'", result.tnf, result.rawType);
  }

  return result;
}

// =============================================================================
// NFC — HELPERS
// =============================================================================

void printUID() {
  Serial.print("UID      : ");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) Serial.print("0");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
    if (i < mfrc522.uid.size - 1) Serial.print(":");
  }
  Serial.println();
}

bool isUltralightFamily() {
  return mfrc522.PICC_GetType(mfrc522.uid.sak) == MFRC522::PICC_TYPE_MIFARE_UL;
}

bool isClassicFamily() {
  MFRC522::PICC_Type t = mfrc522.PICC_GetType(mfrc522.uid.sak);
  return (t == MFRC522::PICC_TYPE_MIFARE_1K  ||
          t == MFRC522::PICC_TYPE_MIFARE_4K  ||
          t == MFRC522::PICC_TYPE_MIFARE_MINI);
}

bool isSameUID(const uint8_t* uid, uint8_t uidLen) {
  if (uidLen != lastUIDLen) return false;
  return memcmp(uid, lastUID, uidLen) == 0;
}

void saveUID(const uint8_t* uid, uint8_t uidLen) {
  lastUIDLen = uidLen;
  memcpy(lastUID, uid, uidLen);
}

// =============================================================================
// SPOTIFY — URL PARSING
// =============================================================================

SpotifyItem parseSpotifyInput(String input) {
  SpotifyItem item = {"", "", false};
  input.trim();

  // Handle full share URLs: https://open.spotify.com/track/...
  int domainPos = input.indexOf("open.spotify.com/");
  if (domainPos != -1) input = input.substring(domainPos + 17);

  // Handle URI format: spotify:track:... → track/...
  if (input.startsWith("spotify:")) {
    input.replace("spotify:", "");
    input.replace(":", "/");
  }

  int slash = input.indexOf('/');
  if (slash == -1) return item;

  item.type     = input.substring(0, slash);
  String rest   = input.substring(slash + 1);
  int    query  = rest.indexOf('?');
  item.id       = (query != -1) ? rest.substring(0, query) : rest;

  item.valid = (item.id.length() > 0) &&
               (item.type == "track"  ||
                item.type == "album"  ||
                item.type == "artist");
  return item;
}

// =============================================================================
// SONOS — SOAP
// =============================================================================

bool soapRequest(String action, String body) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ SOAP failed: no WiFi");
    return false;
  }

  HTTPClient http;
  String url = "http://" + String(SONOS_IP) + ":1400/MediaRenderer/AVTransport/Control";

  http.begin(url);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION",
                 "urn:schemas-upnp-org:service:AVTransport:1#" + action);

  String soap =
    "<?xml version=\"1.0\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>" + body + "</s:Body></s:Envelope>";

  int code = http.POST(soap);
  if (code != 200) {
    Serial.println("❌ SOAP error " + String(code) + " [" + action + "]");
    Serial.println(http.getString());
  }
  http.end();
  return (code == 200);
}

bool sonosPlay() {
  return soapRequest("Play",
    "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID><Speed>1</Speed></u:Play>");
}

bool sonosSetURI(String uri, String metadata) {
  return soapRequest("SetAVTransportURI",
    "<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "<CurrentURI>" + uri + "</CurrentURI>"
    "<CurrentURIMetaData>" + metadata + "</CurrentURIMetaData>"
    "</u:SetAVTransportURI>");
}

bool sonosAddURIToQueue(String uri, String metadata) {
  return soapRequest("AddURIToQueue",
    "<u:AddURIToQueue xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "<EnqueuedURI>" + uri + "</EnqueuedURI>"
    "<EnqueuedURIMetaData>" + metadata + "</EnqueuedURIMetaData>"
    "<DesiredFirstTrackNumberEnqueued>1</DesiredFirstTrackNumberEnqueued>"
    "<EnqueueAsNext>1</EnqueueAsNext>"
    "</u:AddURIToQueue>");
}

bool sonosSeekToQueue() {
  String queueURI = "x-rincon-queue:" + String(SONOS_UDN) + "#0";
  return soapRequest("SetAVTransportURI",
    "<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "<CurrentURI>" + queueURI + "</CurrentURI>"
    "<CurrentURIMetaData></CurrentURIMetaData>"
    "</u:SetAVTransportURI>");
}

bool sonosSeekTrack(int trackNum) {
  return soapRequest("Seek",
    "<u:Seek xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "<Unit>TRACK_NR</Unit>"
    "<Target>" + String(trackNum) + "</Target>"
    "</u:Seek>");
}

// =============================================================================
// SONOS — METADATA BUILDERS
// =============================================================================

String albumMetadata(String id) {
  String itemId = "1004206cspotify%3aalbum%3a" + id;
  return
    "&lt;DIDL-Lite xmlns:dc=&quot;http://purl.org/dc/elements/1.1/&quot; "
    "xmlns:upnp=&quot;urn:schemas-upnp-org:metadata-1-0/upnp/&quot; "
    "xmlns:r=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot; "
    "xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/&quot;&gt;"
    "&lt;item id=&quot;" + itemId + "&quot; parentID=&quot;00020000album:&quot; restricted=&quot;true&quot;&gt;"
    "&lt;dc:title&gt;&lt;/dc:title&gt;"
    "&lt;upnp:class&gt;object.container.album.musicAlbum&lt;/upnp:class&gt;"
    "&lt;desc id=&quot;cdudn&quot; nameSpace=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot;&gt;"
    + String(SONOS_TOKEN) +
    "&lt;/desc&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;";
}

String artistMetadata(String id) {
  String itemId   = "100e206cspotify%3aartistTopTracks%3a" + id;
  String parentId = "10052064spotify%3aartist%3a" + id;
  return
    "&lt;DIDL-Lite xmlns:dc=&quot;http://purl.org/dc/elements/1.1/&quot; "
    "xmlns:upnp=&quot;urn:schemas-upnp-org:metadata-1-0/upnp/&quot; "
    "xmlns:r=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot; "
    "xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/&quot;&gt;"
    "&lt;item id=&quot;" + itemId + "&quot; parentID=&quot;" + parentId + "&quot; restricted=&quot;true&quot;&gt;"
    "&lt;dc:title&gt;Top Tracks&lt;/dc:title&gt;"
    "&lt;upnp:class&gt;object.container.playlistContainer&lt;/upnp:class&gt;"
    "&lt;desc id=&quot;cdudn&quot; nameSpace=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot;&gt;"
    + String(SONOS_TOKEN) +
    "&lt;/desc&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;";
}

// =============================================================================
// SPOTIFY — PLAYBACK
// =============================================================================

void playTrack(String id) {
  String uri = "x-sonos-spotify:spotify%3atrack%3a" + id +
               "?sid=9&amp;flags=8232&amp;sn=" + String(SPOTIFY_SN);
  Serial.println("Loading track...");
  if (sonosSetURI(uri, "") && sonosPlay())
    Serial.println("▶ Playing track: " + id);
}

void playAlbum(String id) {
  String uri = "x-rincon-cpcontainer:1004206cspotify%3aalbum%3a" + id +
               "?sid=9&amp;flags=8300&amp;sn=" + String(SPOTIFY_SN);
  Serial.println("Loading album...");
  if (!sonosAddURIToQueue(uri, albumMetadata(id))) return;
  if (!sonosSeekToQueue())  return;
  if (!sonosSeekTrack(1))   return;
  if (sonosPlay()) Serial.println("▶ Playing album: " + id);
}

void playArtist(String id) {
  String uri = "x-rincon-cpcontainer:100e206cspotify%3aartistTopTracks%3a" + id +
               "?sid=9&amp;flags=8300&amp;sn=" + String(SPOTIFY_SN);
  Serial.println("Loading artist top tracks...");
  if (!sonosAddURIToQueue(uri, artistMetadata(id))) return;
  if (!sonosSeekToQueue())  return;
  if (!sonosSeekTrack(1))   return;
  if (sonosPlay()) Serial.println("▶ Playing artist: " + id);
}

void playSpotifyItem(SpotifyItem item) {
  if      (item.type == "track")  playTrack(item.id);
  else if (item.type == "album")  playAlbum(item.id);
  else if (item.type == "artist") playArtist(item.id);
}
