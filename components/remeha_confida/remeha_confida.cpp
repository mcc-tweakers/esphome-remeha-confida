// ============================================================================
//  Remeha Confida -> ESPHome BLE component  (implementation)
// ----------------------------------------------------------------------------
//  WHAT THIS DOES, END TO END
//  The ESP32 speaks the heat pump's native BLE protocol: it bonds to the
//  heat pump's onboard Bluetooth, passes two challenge/response gates, then
//  reads objects by CANopen-style index/subindex and (optionally) writes them.
//
//  CONNECTION SEQUENCE (state machine St, see .h):
//    1. OPEN            -> request an encrypted (bonded) link (LESC passkey =
//                          the number on the appliance's BLE sticker).
//    2. SEARCH_CMPL     -> resolve the GATT handles (resolve_handles_).
//    3. enable notifies -> on 4 characteristics (start_notify_); when all CCCDs
//                          are written we start the handshake.
//    4. GATE handshake  -> a 0-byte write to 0x1B makes the module send an
//                          8-byte challenge on 0x1D; we answer on 0x20 with a
//                          TEA response (tea_); it acks on 0x22.  (gate_trigger_)
//    5. CANIP handshake -> session-start + a second TEA challenge/response over
//                          the CANIP data channel (canip_handshake_).
//    6. RUN             -> normal polling: read everything, write when asked.
//
//  WIRE FRAME (all data rides the CANIP write/notify chars, handles 0x0E/0x10):
//    message = 01 02 00 00 00 | OP | RESV | <op-specific payload> | CRChi CRClo
//      - "01 02 00 00 00" is the 5-byte start marker of every message.
//      - OP (offset 5): 0x00 read-req, 0x01 read-resp, 0x04 write-req,
//                       0x05 write-resp, 0x08 GetList-req, 0x09 GetList-resp.
//      - CRC is CRC-16/MODBUS over everything before it, stored BIG-endian.
//    There is NO extra 0xFF in front of a message: the 0xFF seen at the start
//    of a single-fragment write/read is the fragment seq byte below.
//
//  FRAGMENTATION: every message is split into <=20-byte BLE notifications.
//    Each fragment is [seq][payload]; seq counts 0,1,2,... and the LAST
//    fragment uses seq=0xFF. We strip the seq byte and concatenate payloads
//    until we see 0xFF (send_next_fragment_ on TX, NOTIFY handler on RX).
//
//  OBJECTS: each value is addressed by (node, index, subindex). Many objects
//    are ARRAYS (MaxArraySize=5) whose real value lives at subindex >= 1, not
//    0 -- e.g. zone objects at sub 1 (active zone CIRCA), energy totals at
//    sub 1. Reading the wrong subindex returns a 0/sentinel. See RemehaItem.
//
//  TWO READ PATHS:
//    - BULK (default): one GetList request lists every bulk object; the reply
//      packs all of them (no per-record CRC) and is reassembled from fragments.
//    - SINGLE (read: single): an individual read request (op 0x00) per object,
//      for scalars the appliance does not surface in the bulk GetList. Chained
//      one-after-each-response so they finish inside one poll cycle.
//
//  WRITE: RemehaNumber::control -> enqueue_write builds an op 0x04 frame; it is
//    sent before the next poll; the appliance replies with an op 0x05 resp.
// ============================================================================
#ifdef USE_ESP32
#include "remeha_confida.h"
#include "esphome/core/log.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <esp_err.h>

namespace esphome {
namespace remeha_confida {

// CRC-16/MODBUS (poly 0xA001, init 0xFFFF), stored big-endian on the wire.

static const char *const TAG = "remeha_confida";
static const size_t FRAG_DATA = 19;

// RUN-phase watchdog thresholds (ms). The poll interval is 30 s, so RUN_RX_TIMEOUT
// is ~6 missed polls of silence before we give up on the link; TX_STUCK is how long
// a single request may sit half-sent (fragment write never acked) before reconnect.
static const uint32_t RUN_RX_TIMEOUT_MS = 180000;  // 3 min with no response -> reconnect
static const uint32_t TX_STUCK_MS = 15000;         // TX latched 15 s -> reconnect

// ---------------------------------------------------------------------------
// PAIRING RULES of the appliance's BLE gateway (GTW-Blue, node 5). These are the
// gateway's own rules, observed from its behaviour:
//  1. After a WRONG PIN the gateway is "not available for 60 seconds". Any new
//     pairing attempt in that window is refused with SMP "Repeated Attempts"
//     (ESP auth fail 0x56). Retrying every few seconds keeps it locked forever,
//     so after a failure we stay quiet for at least PAIR_MIN_GAP_MS.
//  2. It keeps at most 3 pairings; pairing a 4th device makes it forget the
//     OLDEST. When that is the ESP, our stored key is stale: encryption with it
//     fails. Then we delete our own bond once so the next attempt pairs afresh.
//  3. An aborted pairing may count as a failed one, so the watchdog never cuts
//     off a pairing that is still running (PAIR_TIMEOUT_MS).
// ---------------------------------------------------------------------------
static const uint32_t PAIR_MIN_GAP_MS = 90000;   // > the gateway's 60 s lock-out
static const uint32_t PAIR_TIMEOUT_MS = 30000;   // let a running pairing finish
static const uint32_t PAIR_BACKOFF_S[] = {90, 180, 300, 600};  // grows per consecutive failure

// ESP-IDF reports auth_cmpl.fail_reason as 77 (0x4D) + SMP reason (Core spec
// Vol 3 Part H 3.5.5), followed by stack-internal codes. E.g. 0x56 = 86 = 77 + 9.
static const char *auth_fail_str(uint8_t r) {
  switch (r) {
    case 0x4E: return "Passkey Entry Failed";
    case 0x50: return "Authentication Requirements";
    case 0x51: return "Confirm Value Failed - passkey klopt niet?";
    case 0x52: return "Pairing Not Supported";
    case 0x53: return "Encryption Key Size";
    case 0x55: return "Unspecified Reason";
    case 0x56: return "Repeated Attempts - gateway geblokkeerd na mislukte poging";
    case 0x57: return "Invalid Parameters";
    case 0x58: return "DHKey Check Failed";
    case 0x60: return "SMP busy";
    case 0x61: return "Encryption failed - gateway kent ons niet meer?";
    case 0x63: return "SMP response timeout";
    case 0x66: return "connection timeout";
    default:   return "zie ESP-IDF esp_ble_auth_fail_rsn_t";
  }
}
// CANopen SDO abort codes, as carried in the appliance's error responses
// (op 0x02 = read failed, op 0x06 = write failed; 4 bytes little-endian).
static const char *sdo_abort_str(uint32_t c) {
  switch (c) {
    case 0x05040001: return "ongeldig commando";
    case 0x06010000: return "geen toegang tot object";
    case 0x06010001: return "object is alleen-schrijven";
    case 0x06010002: return "object is alleen-lezen";
    case 0x06020000: return "object bestaat niet (op deze node)";
    case 0x06040041: return "object kan niet gemapt worden";
    case 0x06040043: return "parameter niet compatibel";
    case 0x06040047: return "interne incompatibiliteit";
    case 0x06060000: return "hardwarefout bij toegang";
    case 0x06070010: return "datatype/lengte klopt niet";
    case 0x06070012: return "datatype klopt niet, te lang";
    case 0x06070013: return "datatype klopt niet, te kort";
    case 0x06090011: return "subindex bestaat niet";
    case 0x06090030: return "waarde buiten toegestaan bereik";
    case 0x06090031: return "waarde te hoog";
    case 0x06090032: return "waarde te laag";
    case 0x06090036: return "maximum kleiner dan minimum";
    case 0x08000000: return "algemene fout";
    case 0x08000020: return "kan niet opslaan";
    case 0x08000021: return "kan niet opslaan (lokale besturing)";
    case 0x08000022: return "kan niet opslaan (huidige toestand van het toestel)";
    case 0x08000024: return "geen data beschikbaar";
    default:         return "onbekende abortcode";
  }
}

// Failures that say nothing about the stored key: never delete the bond for these.
static bool transient_fail(uint8_t r) { return r == 0x60 || r == 0x63 || r == 0x66; }

// Fixed UUIDs of the Confida BLE module's "gate" service + characteristics.
// These are the anchors: handles are looked up from the GATT database by UUID
// at connect time (resolve_handles_), so nothing here depends on fixed handles.
static const char *GATE_SVC = "6a37b97e-779d-457f-8182-edf334edd01f";
static const char *GATE_1B  = "94e54dc4-f69a-4c0c-8a6f-cc8f3135c665";  // write (trigger)
static const char *GATE_1D  = "69092300-a182-4fea-803b-24ada406da8a";  // notify (challenge)
static const char *GATE_20  = "f1ca5aeb-02cd-44a8-972b-44b6e7250d0b";  // write (response)
static const char *GATE_22  = "e2c47825-6dff-4d79-b74a-5bdc4f4661cf";  // notify (ack)
// Second notify characteristic inside the CANIP service (props 0x18 =
// notify+indicate) that is also subscribed to. Fixed UUID; handle resolved.
static const char *CANIP_EXTRA = "cff32957-5279-4075-bf3d-8e583d418c0d";

uint16_t RemehaConfida::crc_modbus_(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

// TEA encrypt of block (level,level), key [0x15EFA43F, serial, chal, level], 32 rounds.
void RemehaConfida::tea_(uint8_t level, uint32_t serial, uint32_t chal, uint32_t &o1, uint32_t &o2) {
  const uint32_t delta = 0x9E3779B9u, k0 = 0x15EFA43Fu;
  uint32_t v0 = level, v1 = level, s = delta;
  for (int i = 0; i < 32; i++) {
    v0 += ((k0 + (v1 << 4)) ^ (serial + (v1 >> 5)) ^ (v1 + s));
    v1 += ((chal + (v0 << 4)) ^ (s + v0) ^ ((uint32_t) level + (v0 >> 5)));
    s += delta;
  }
  o1 = v0; o2 = v1;
}

static std::string tohex(const uint8_t *d, size_t n) {
  static const char *H = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s.push_back(H[d[i] >> 4]); s.push_back(H[d[i] & 0xF]); }
  return s;
}
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
static uint32_t be32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

void RemehaConfida::setup() {
  gate_svc_ = espbt::ESPBTUUID::from_raw(GATE_SVC);
  gate_1b_ = espbt::ESPBTUUID::from_raw(GATE_1B);
  gate_1d_ = espbt::ESPBTUUID::from_raw(GATE_1D);
  gate_20_ = espbt::ESPBTUUID::from_raw(GATE_20);
  gate_22_ = espbt::ESPBTUUID::from_raw(GATE_22);
  canip_extra_ = espbt::ESPBTUUID::from_raw(CANIP_EXTRA);
  esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_IN;
  uint8_t ks = 16, ik = 0x0F, rk = 0x0F;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &ks, sizeof(ks));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &ik, sizeof(ik));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rk, sizeof(rk));
  if (has_passkey_) { uint32_t pk = passkey_; esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &pk, sizeof(pk)); }

  // Auto-connect mode: if the ble_client MAC is left at 00:00:00:00:00:00, we
  // discover the appliance by advertised-name prefix. Register as a scan
  // listener so parse_device() sees every advertisement. With a real MAC filled
  // in, ble_client handles the connection itself and we skip scanning entirely.
  uint64_t cfg_addr = this->parent()->get_address();
  scan_mode_ = (cfg_addr == 0);
  if (!scan_mode_) addr_resolved_ = true;
  // The scan-listener is registered at build time (see __init__.py), so
  // parse_device() receives advertisements. Logged at INFO (not CONFIG) so it is
  // always captured in the live log.
  ESP_LOGI(TAG, "init: geconfigureerd MAC=0x%012llX  scan_mode=%d  prefix='%s'",
           (unsigned long long) cfg_addr, (int) scan_mode_, name_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "Remeha Confida set up (%d items)", (int) items_.size());
}

bool RemehaConfida::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  uint64_t addr = device.address_uint64();
  const std::string &name = device.get_name();

  // Log every distinct BLE device once (diagnostics + so a user with multiple
  // appliances can read off which MAC to put in the YAML). Always, so that the
  // log confirms the scan listener is actually receiving advertisements.
  if (seen_.find(addr) == seen_.end()) {
    seen_.insert(addr);
    ESP_LOGI(TAG, "BLE gezien: %s  naam='%s'  rssi=%d",
             device.address_str().c_str(), name.c_str(), device.get_rssi());
  }

  // Only in scan-mode: first device whose name starts with the prefix -> pin its
  // MAC on the ble_client and let ble_client connect on the next advertisement.
  if (scan_mode_ && !addr_resolved_ && !name.empty() && name.rfind(name_prefix_, 0) == 0) {
    addr_resolved_ = true;
    this->parent()->set_address(addr);
    ESP_LOGI(TAG, "Confida gevonden: %s (%s) - verbinden",
             name.c_str(), device.address_str().c_str());
  }
  return false;  // never claim the advertisement; ble_client drives the connection
}

void RemehaConfida::dump_config() {
  ESP_LOGCONFIG(TAG, "Remeha Confida: items=%d", (int) items_.size());
  // Show only the last two digits: enough to spot a wrong/mangled secret
  // (e.g. a leading zero read as octal) without putting the PIN in the log.
  if (has_passkey_)
    ESP_LOGCONFIG(TAG, "  passkey: ****%02u (laatste 2 cijfers, controleer tegen de sticker)",
                  (unsigned) (passkey_ % 100));
  else
    ESP_LOGW(TAG, "  GEEN passkey ingesteld - koppelen gaat mislukken");
}

void RemehaConfida::publish_reason_(const char *why) {
  if (reason_ts_ != nullptr) reason_ts_->publish_state(why);
}

// After a failed pairing/encryption: drop the link, stop ble_client from
// reconnecting, and wait (see PAIRING RULES). If the failure happened while we
// still hold a bond for the appliance, that key is stale (rule 2): delete it so
// the next attempt is a fresh passkey pairing - but only after the wait.
void RemehaConfida::pairing_failed_(const uint8_t *bda, uint8_t reason) {
  bool had_bond = false;
  int n = esp_ble_get_bond_device_num();
  if (n > 0) {
    std::vector<esp_ble_bond_dev_t> list(n);
    if (esp_ble_get_bond_device_list(&n, list.data()) == ESP_OK) {
      for (int i = 0; i < n; i++)
        if (memcmp(list[i].bd_addr, bda, 6) == 0) had_bond = true;
    }
  }
  if (had_bond && !transient_fail(reason)) {
    ESP_LOGW(TAG, "opgeslagen sleutel werkt niet meer (gateway heeft ons vergeten?) - bond wissen, daarna vers koppelen");
    esp_bd_addr_t a;
    memcpy(a, bda, 6);
    esp_ble_remove_bond_device(a);
  } else if (reason == 0x51 || reason == 0x4E) {
    ESP_LOGE(TAG, "passkey lijkt niet te kloppen - controleer remeha_bt_key in secrets.yaml");
  }

  const size_t nb = sizeof(PAIR_BACKOFF_S) / sizeof(PAIR_BACKOFF_S[0]);
  const uint32_t wait_s = PAIR_BACKOFF_S[pair_fails_ < nb ? pair_fails_ : nb - 1];
  if (pair_fails_ < 255) pair_fails_++;
  last_fail_ = millis();
  ever_failed_ = true;
  backoff_ = true;
  backoff_until_ = last_fail_ + wait_s * 1000UL;
  pairing_ = false;
  reconnects_++;
  char msg[96];
  snprintf(msg, sizeof(msg), "koppelen mislukt 0x%02X - wacht %u s", reason, (unsigned) wait_s);
  ESP_LOGW(TAG, "%s (poging %u op rij)", msg, (unsigned) pair_fails_);
  publish_reason_(msg);
  if (reconnect_sensor_ != nullptr) reconnect_sensor_->publish_state((float) reconnects_);
  st_ = ST_INIT;
  tx_busy_ = false;
  rx_msg_.clear(); indiv_q_.clear();
  this->parent()->set_enabled(false);  // disconnects AND stops ble_client from reconnecting
}

void RemehaConfida::write_raw_(uint16_t handle, const uint8_t *d, size_t n) {
  esp_err_t e = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                         handle, n, const_cast<uint8_t *>(d),
                                         ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
  if (e != ESP_OK)
    ESP_LOGW(TAG, "write_char(0x%04X, %d bytes) returned %s", handle, (int) n, esp_err_to_name(e));
}

// Dump the complete GATT attribute database (services, characteristics,
// descriptors) with real handles and 128-bit UUIDs, for diagnostics. The
// actual handle resolution happens in resolve_handles_(); this is read-only.
void RemehaConfida::dump_gatt_() {
  auto if_ = this->parent()->get_gattc_if();
  auto conn = this->parent()->get_conn_id();
  uint16_t count = 0;
  esp_gatt_status_t s = esp_ble_gattc_get_attr_count(if_, conn, ESP_GATT_DB_ALL, 0x0001, 0xFFFF, 0, &count);
  if (s != ESP_GATT_OK || count == 0) { ESP_LOGW(TAG, "get_attr_count st=%d count=%d", s, count); return; }
  esp_gattc_db_elem_t *db = (esp_gattc_db_elem_t *) malloc(sizeof(esp_gattc_db_elem_t) * count);
  if (db == nullptr) { ESP_LOGW(TAG, "db malloc failed (%d attrs)", count); return; }
  uint16_t got = count;
  s = esp_ble_gattc_get_db(if_, conn, 0x0001, 0xFFFF, db, &got);
  if (s != ESP_GATT_OK) { ESP_LOGW(TAG, "get_db st=%d", s); free(db); return; }
  ESP_LOGI(TAG, "==== GATT DB: %d attributes ====", got);
  for (uint16_t i = 0; i < got; i++) {
    esp_gattc_db_elem_t &e = db[i];
    char u[40];
    if (e.uuid.len == ESP_UUID_LEN_16) snprintf(u, sizeof(u), "0x%04X", e.uuid.uuid.uuid16);
    else if (e.uuid.len == ESP_UUID_LEN_32) snprintf(u, sizeof(u), "0x%08lX", (unsigned long) e.uuid.uuid.uuid32);
    else {
      const uint8_t *p = e.uuid.uuid.uuid128;  // stored little-endian
      snprintf(u, sizeof(u), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               p[15], p[14], p[13], p[12], p[11], p[10], p[9], p[8], p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    }
    const char *t = e.type == ESP_GATT_DB_PRIMARY_SERVICE ? "SVC" :
                    e.type == ESP_GATT_DB_SECONDARY_SERVICE ? "SV2" :
                    e.type == ESP_GATT_DB_CHARACTERISTIC ? "CHR" :
                    e.type == ESP_GATT_DB_DESCRIPTOR ? "DSC" : "oth";
    ESP_LOGI(TAG, "  %s h=0x%04X props=0x%02X uuid=%s", t, e.attribute_handle, e.properties, u);
  }
  free(db);
}

// Resolve every characteristic + CCCD handle dynamically from the GATT database
// by matching each characteristic's UUID. Nothing here depends on fixed handle
// numbers: the CANIP write/notify + the extra CANIP notify characteristic and the
// four gate characteristics are located by UUID, and each notify characteristic's
// CCCD (the 0x2902 descriptor that follows it) is taken from the next attribute.
// This keeps the component working even if an appliance lays out its attribute
// table with different handles. Safe to call repeatedly: returns false until
// service discovery is ready, so poll() retries on the next round.
bool RemehaConfida::resolve_handles_() {
  if (h_canip_w_ != 0) return true;  // already resolved

  auto if_ = this->parent()->get_gattc_if();
  auto conn = this->parent()->get_conn_id();
  uint16_t count = 0;
  esp_gatt_status_t s = esp_ble_gattc_get_attr_count(if_, conn, ESP_GATT_DB_ALL, 0x0001, 0xFFFF, 0, &count);
  if (s != ESP_GATT_OK || count == 0) return false;  // discovery not complete yet
  esp_gattc_db_elem_t *db = (esp_gattc_db_elem_t *) malloc(sizeof(esp_gattc_db_elem_t) * count);
  if (db == nullptr) { ESP_LOGW(TAG, "db malloc failed (%d attrs)", count); return false; }
  uint16_t got = count;
  s = esp_ble_gattc_get_db(if_, conn, 0x0001, 0xFFFF, db, &got);
  if (s != ESP_GATT_OK) { ESP_LOGW(TAG, "get_db st=%d", s); free(db); return false; }

  // Each characteristic UUID we need, with where to store its value handle and
  // (for notify characteristics) its CCCD handle.
  struct Target { const espbt::ESPBTUUID &uuid; uint16_t *h; uint16_t *cccd; };
  Target targets[] = {
    { write_uuid_,  &h_canip_w_, nullptr },
    { notify_uuid_, &h_canip_n_, &cccd_canip_ },
    { canip_extra_, &h_13_,      &cccd_13_ },
    { gate_1b_,     &h_1b_,      nullptr },
    { gate_1d_,     &h_1d_,      &cccd_1d_ },
    { gate_20_,     &h_20_,      nullptr },
    { gate_22_,     &h_22_,      &cccd_22_ },
  };
  const int NT = sizeof(targets) / sizeof(targets[0]);
  for (int k = 0; k < NT; k++) { *targets[k].h = 0; if (targets[k].cccd) *targets[k].cccd = 0; }

  int cur = -1;  // index of the target matched by the characteristic we are inside
  for (uint16_t i = 0; i < got; i++) {
    esp_gattc_db_elem_t &e = db[i];
    if (e.type == ESP_GATT_DB_CHARACTERISTIC) {
      cur = -1;
      espbt::ESPBTUUID cu = espbt::ESPBTUUID::from_uuid(e.uuid);
      for (int k = 0; k < NT; k++) {
        if (cu == targets[k].uuid) { *targets[k].h = e.attribute_handle; cur = k; break; }
      }
    } else if (e.type == ESP_GATT_DB_DESCRIPTOR &&
               e.uuid.len == ESP_UUID_LEN_16 && e.uuid.uuid.uuid16 == 0x2902) {
      if (cur >= 0 && targets[cur].cccd != nullptr) *targets[cur].cccd = e.attribute_handle;
    }
  }
  free(db);

  // CANIP read/write is mandatory; without it there is nothing to do, so retry.
  // Leave every handle at 0 so poll()'s `h_1b_ == 0` retry gate stays consistent.
  if (h_canip_w_ == 0 || h_canip_n_ == 0) {
    ESP_LOGW(TAG, "GATT: CANIP-characteristics nog niet gevonden (w=0x%04X n=0x%04X)", h_canip_w_, h_canip_n_);
    for (int k = 0; k < NT; k++) { *targets[k].h = 0; if (targets[k].cccd) *targets[k].cccd = 0; }
    return false;
  }
  if (h_1b_ == 0 || h_1d_ == 0 || h_20_ == 0 || h_22_ == 0)
    ESP_LOGW(TAG, "GATT: niet alle gate-characteristics gevonden (1b=0x%04X 1d=0x%04X 20=0x%04X 22=0x%04X)",
             h_1b_, h_1d_, h_20_, h_22_);

  // Print the full GATT table once per connection (alle service- en characteristic-
  // UUID's), zodat je je eigen Confida-UUID's rechtstreeks uit de log kunt aflezen/
  // verifieren - net als een nRF Connect-scan, maar zonder telefoon.
  if (!dumped_) { dump_gatt_(); dumped_ = true; }

  ESP_LOGI(TAG, "handles (dynamisch): canip w=0x%04X n=0x%04X extra=0x%04X (cccd n=0x%04X extra=0x%04X) | "
                "gate 1b=0x%04X 1d=0x%04X 20=0x%04X 22=0x%04X (cccd 1d=0x%04X 22=0x%04X)",
           h_canip_w_, h_canip_n_, h_13_, cccd_canip_, cccd_13_,
           h_1b_, h_1d_, h_20_, h_22_, cccd_1d_, cccd_22_);
  start_notify_();
  return true;
}

void RemehaConfida::start_notify_() {
  reg_done_ = descr_done_ = 0;
  auto if_ = this->parent()->get_gattc_if();
  auto bda = this->parent()->get_remote_bda();
  const uint16_t hs[] = {h_1d_, h_22_, h_canip_n_, h_13_};
  notify_expected_ = 0;
  for (uint16_t h : hs) if (h != 0) notify_expected_++;
  ESP_LOGI(TAG, "enabling notifications on 0x%04X, 0x%04X, 0x%04X, 0x%04X", h_1d_, h_22_, h_canip_n_, h_13_);
  for (uint16_t h : hs) if (h != 0) esp_ble_gattc_register_for_notify(if_, bda, h);
}

void RemehaConfida::gate_trigger_() {
  st_ = ST_GATE_WAIT_CHAL;
  rx_msg_.clear();
  // The appliance issues the challenge only on a ZERO-length Write Request to
  // 0x1B. esp_ble_gattc_write_char refuses len 0, but write_char_descr accepts
  // (value==nullptr && len==0) and emits the same ATT Write Request (0x12) to
  // the handle - on air this is "12 1b 00".
  ESP_LOGI(TAG, ">> gate: trigger (0-byte write to 0x%04X)", h_1b_);
  esp_err_t e = esp_ble_gattc_write_char_descr(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                               h_1b_, 0, nullptr, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
  if (e != ESP_OK) ESP_LOGW(TAG, "gate write_char_descr returned %s", esp_err_to_name(e));
}

void RemehaConfida::canip_start_() {
  st_ = ST_CANIP_WAIT_CHAL;
  rx_msg_.clear();
  static const uint8_t f[] = {0xff, 0x01, 0x02, 0x01, 0x00, 0x02, 0x00, 0x03, 0x23, 0xd6};
  ESP_LOGI(TAG, ">> canip: session start");
  write_raw_(h_canip_w_, f, sizeof(f));
}

void RemehaConfida::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, passkey_); break;
    case ESP_GAP_BLE_NC_REQ_EVT:      esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true); break;
    case ESP_GAP_BLE_SEC_REQ_EVT:     esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true); break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      // This event reaches us twice (esp32_ble AND ble_client): handle it once.
      if (auth_handled_) break;
      auth_handled_ = true;
      pairing_ = false;
      const auto &ac = param->ble_security.auth_cmpl;
      if (ac.success) {
        ESP_LOGI(TAG, "pairing SUCCESS");
        pair_fails_ = 0;
        if (gate_pending_ && st_ == ST_INIT) {
          gate_pending_ = false;
          ESP_LOGI(TAG, "pairing done - starting gate handshake");
          gate_trigger_();
        }
      } else {
        ESP_LOGE(TAG, "pairing FAILED 0x%02X = %s", ac.fail_reason, auth_fail_str(ac.fail_reason));
        if (active_) pairing_failed_(ac.bd_addr, ac.fail_reason);
      }
      break;
    }
    default: break;
  }
}

void RemehaConfida::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (!active_) {  // released while a connect slipped through: drop it again
        // ...and stop ble_client from reconnecting. Needed after boot with the switch
        // restored to OFF: set_active(false) then ran before ble_client's own setup
        // re-enabled it, giving a connect/close loop every ~0.7 s.
        esp_ble_gattc_close(this->parent()->get_gattc_if(), this->parent()->get_conn_id());
        this->parent()->set_enabled(false);
        break;
      }
      if (param->open.status != ESP_GATT_OK) break;
      auth_handled_ = false;
      gate_pending_ = false;
      pairing_ = true;
      pair_start_ = millis();
      esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      ESP_LOGI(TAG, "SEARCH_CMPL received - resolving handles");
      resolve_handles_();
      break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      // ESPHome's BLEClientBase writes the CCCD itself on this event, so we only
      // count registrations here (writing it again just doubles the traffic).
      reg_done_++;
      ESP_LOGD(TAG, "reg_for_notify ok ch=0x%04X (%d/%d)", param->reg_for_notify.handle, reg_done_, notify_expected_);
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      if (st_ == ST_INIT) {
        descr_done_++;
        ESP_LOGD(TAG, "CCCD %d/%d status 0x%02X", descr_done_, notify_expected_, param->write.status);
        if (descr_done_ >= notify_expected_) {
          if (pairing_) {
            // A fresh passkey pairing is still running. The gate ignores a trigger on
            // a link that is not encrypted yet (the handshake then stalls and costs a
            // reconnect), so start it from AUTH_CMPL instead.
            if (!gate_pending_) ESP_LOGI(TAG, "notifications enabled - gate waits for pairing");
            gate_pending_ = true;
          } else {
            ESP_LOGI(TAG, "notifications enabled - starting gate handshake");
            gate_trigger_();
          }
        }
      } else {
        ESP_LOGD(TAG, "descr write 0x%04X status 0x%02X", param->write.handle, param->write.status);
      }
      break;
    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.status != 0) {
        ESP_LOGW(TAG, "write 0x%04X status 0x%02X", param->write.handle, param->write.status);
      } else if (st_ != ST_RUN) {
        ESP_LOGD(TAG, "write 0x%04X ok", param->write.handle);
      }
      if (st_ == ST_RUN && tx_busy_) send_next_fragment_();
      break;
    case ESP_GATTC_NOTIFY_EVT: {
      uint16_t h = param->notify.handle;
      const uint8_t *v = param->notify.value;
      uint16_t n = param->notify.value_len;
      ESP_LOGD(TAG, "<< NOTIFY h=0x%04X (%d) %s", h, n, tohex(v, n).c_str());
      if (h == h_1d_ && st_ == ST_GATE_WAIT_CHAL && n >= 8) {
        uint32_t serial = le32(v), chal = le32(v + 4), w1, w2;
        tea_(auth_level_, serial, chal, w1, w2);
        uint8_t resp[9] = {auth_level_,
                           (uint8_t) w1, (uint8_t)(w1 >> 8), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 24),
                           (uint8_t) w2, (uint8_t)(w2 >> 8), (uint8_t)(w2 >> 16), (uint8_t)(w2 >> 24)};
        ESP_LOGI(TAG, ">> gate: challenge %s -> response %s", tohex(v, 8).c_str(), tohex(resp, 9).c_str());
        st_ = ST_GATE_WAIT_ACK;
        write_raw_(h_20_, resp, sizeof(resp));
      } else if (h == h_22_ && st_ == ST_GATE_WAIT_ACK) {
        ESP_LOGI(TAG, ">> gate: ack %s - gate passed", tohex(v, n).c_str());
        pairing_ = false;  // the gate only answers on an encrypted link, so pairing is done
        canip_start_();
      } else if (h == h_canip_n_) {
        if (n < 1) break;
        uint8_t seq = v[0];
        rx_msg_.insert(rx_msg_.end(), v + 1, v + n);
        if (seq != 0xFF) break;
        auto m = rx_msg_; rx_msg_.clear();
        if (st_ == ST_RUN) {
          responses_++;
          last_rx_ = millis();   // link is alive (feeds the RUN watchdog)
          parse_response_(m);
          // Chain the next single-read request right after this response so all
          // individual reads finish within the current poll cycle (not one per tick).
          send_next_individual_();
        } else { ESP_LOGD(TAG, "<< canip %s", tohex(m.data(), m.size()).c_str()); canip_handshake_(m); }
      }
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "disconnected");
      pairing_ = false;
      gate_pending_ = false;
      st_ = ST_INIT; h_canip_w_ = h_canip_n_ = h_1b_ = h_1d_ = h_20_ = h_22_ = h_13_ = 0;
      cccd_1d_ = cccd_22_ = cccd_canip_ = cccd_13_ = 0;
      reg_done_ = descr_done_ = notify_expected_ = 0; dumped_ = false;
      tx_busy_ = false; rx_msg_.clear(); tx_msg_.clear(); tx_queue_.clear(); indiv_q_.clear();
      break;
    default: break;
  }
}

void RemehaConfida::canip_handshake_(const std::vector<uint8_t> &m) {
  if (m.size() < 7) return;
  uint8_t cls = m[4], step = m[5];
  if (cls == 0x02 && step == 0x01 && m.size() >= 15) {
    uint32_t serial = be32(&m[7]), chal = be32(&m[11]), w1, w2;
    tea_(auth_level_, serial, chal, w1, w2);
    ESP_LOGI(TAG, ">> canip: challenge serial=%08lX chal=%08lX -> authorize", (unsigned long) serial, (unsigned long) chal);
    uint8_t body[17] = {0x01, 0x02, 0x01, 0x00, 0x02, 0x02, 0x03, 0xff, auth_level_,
                        (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t) w1,
                        (uint8_t)(w2 >> 24), (uint8_t)(w2 >> 16), (uint8_t)(w2 >> 8), (uint8_t) w2};
    uint16_t c = crc_modbus_(body, sizeof(body));
    uint8_t f[20]; f[0] = 0xff; memcpy(f + 1, body, sizeof(body));
    f[18] = (uint8_t)(c >> 8); f[19] = (uint8_t)(c & 0xFF);
    st_ = ST_CANIP_WAIT_ACK;
    write_raw_(h_canip_w_, f, sizeof(f));
  } else if (cls == 0x02 && step == 0x03) {
    static const uint8_t f[] = {0xff, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x03, 0xff, 0x58, 0x2a, 0x00, 0xe5, 0x44};
    ESP_LOGI(TAG, ">> canip: authorized - follow-up");
    st_ = ST_CANIP_WAIT_RX6;
    write_raw_(h_canip_w_, f, sizeof(f));
  } else if (cls == 0x00 && step == 0x02) {
    st_ = ST_RUN;
    last_rx_ = millis();   // arm the RUN watchdog from a fresh timestamp
    // Publish "verbonden" once (first connect) so the reason entity isn't empty and
    // shows a clean baseline; after that it holds the LAST reconnect reason.
    if (reason_ts_ != nullptr && !connected_once_) reason_ts_->publish_state("verbonden");
    if (reconnect_sensor_ != nullptr && !connected_once_) reconnect_sensor_->publish_state(0);
    connected_once_ = true;
    ESP_LOGI(TAG, ">> handshake complete - starting GetList polling");
  } else {
    ESP_LOGW(TAG, "unexpected canip msg cls=0x%02X step=0x%02X", cls, step);
  }
}

void RemehaConfida::build_request_() {
  std::vector<uint8_t> &m = tx_msg_; m.clear();
  // Bulk-read items only; single-read items are fetched one by one (see queue_individual_reads_).
  uint8_t count = 0;
  for (auto &it : items_) { if (!it.single && count < 255) count++; }
  m.insert(m.end(), {0x01, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, count});
  uint8_t done = 0;
  for (auto &it : items_) {
    if (it.single || done >= count) continue;
    m.push_back(0x00); m.push_back(it.node);
    m.push_back((uint8_t)(it.index >> 8)); m.push_back((uint8_t)(it.index & 0xFF)); m.push_back(it.sub);
    done++;
  }
  uint16_t c = crc_modbus_(m.data(), m.size());
  m.push_back((uint8_t)(c >> 8)); m.push_back((uint8_t)(c & 0xFF));
}

// Build an individual READ request (operation 0x00) for one object:
// 01 02 00 00 00 00 00 <node> <idxHi> <idxLo> <sub> <crcHi> <crcLo>.
// NO leading 0xFF: the 0xFF seen on the wire is the fragment seq byte
// (last fragment), which send_next_fragment_ adds. (Until 16 okt 2026 we added
// an extra 0xFF here, so on air it read "FF FF 01 02 ..." and the appliance
// silently ignored every single read and every write.)
std::vector<uint8_t> RemehaConfida::build_read_frame_(uint8_t node, uint16_t index, uint8_t sub) {
  std::vector<uint8_t> body = {0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, node,
                               (uint8_t)(index >> 8), (uint8_t)(index & 0xFF), sub};
  uint16_t c = crc_modbus_(body.data(), body.size());
  std::vector<uint8_t> frame;
  frame.reserve(body.size() + 2);
  frame.insert(frame.end(), body.begin(), body.end());
  frame.push_back((uint8_t)(c >> 8));
  frame.push_back((uint8_t)(c & 0xFF));
  return frame;
}

// (Re)fill the individual-read queue: one request frame per single-read item.
void RemehaConfida::queue_individual_reads_() {
  indiv_q_.clear();
  for (auto &it : items_) {
    if (it.single) indiv_q_.push_back(build_read_frame_(it.node, it.index, it.sub));
  }
}

// Pop and send the next queued individual read. Returns false when the queue is empty.
bool RemehaConfida::send_next_individual_() {
  if (indiv_q_.empty() || tx_busy_) return false;
  auto f = indiv_q_.front();
  indiv_q_.erase(indiv_q_.begin());
  ESP_LOGD(TAG, "sending individual read (%d bytes), %d left", (int) f.size(), (int) indiv_q_.size());
  start_tx_(std::move(f));
  return true;
}

// Switch handler: release the BLE link (so a phone or service tool can connect) or resume.
void RemehaConfida::set_active(bool a) {
  if (a == active_) return;
  active_ = a;
  if (a) {
    // Manual "on" skips the remaining back-off, but never re-enters the gateway's
    // 60 s lock-out window after a failed pairing (that would only extend it).
    if (ever_failed_ && millis() - last_fail_ < PAIR_MIN_GAP_MS) {
      backoff_ = true;
      backoff_until_ = last_fail_ + PAIR_MIN_GAP_MS;
      ESP_LOGI(TAG, "uitlezing INGESCHAKELD - wacht eerst tot %u s na de mislukte koppeling",
               (unsigned) (PAIR_MIN_GAP_MS / 1000));
      publish_reason_("wacht na mislukte koppeling");
      return;
    }
    backoff_ = false;
    ESP_LOGI(TAG, "uitlezing INGESCHAKELD - opnieuw verbinden");
    publish_reason_("verbonden");
    this->parent()->set_enabled(true);   // ble_client mag weer verbinden
  } else {
    ESP_LOGW(TAG, "uitlezing VRIJGEGEVEN - BLE loslaten (telefoon/installateur kan verbinden)");
    if (reason_ts_ != nullptr) reason_ts_->publish_state("vrijgegeven");
    st_ = ST_INIT;
    tx_busy_ = false; rx_msg_.clear(); indiv_q_.clear(); tx_queue_.clear();
    this->parent()->set_enabled(false);  // verbreekt de verbinding EN stopt herverbinden
  }
}

void RemehaConfida::update() {
  if (!active_) {        // released: don't drive the connection
    if (this->parent()->enabled) this->parent()->set_enabled(false);
    return;
  }
  if (backoff_) {        // waiting after a failed pairing (PAIRING RULES 1)
    if ((int32_t) (millis() - backoff_until_) >= 0) {
      backoff_ = false;
      ESP_LOGI(TAG, "wachttijd voorbij - nieuwe koppelpoging");
      publish_reason_("nieuwe koppelpoging");
      this->parent()->set_enabled(true);
    }
    return;
  }
  if (pairing_) {        // pairing/encryption still running: never cut it off (RULES 3)
    if (millis() - pair_start_ < PAIR_TIMEOUT_MS) return;
    ESP_LOGW(TAG, "koppelen geeft na %u s geen resultaat", (unsigned) (PAIR_TIMEOUT_MS / 1000));
    pairing_failed_(this->parent()->get_remote_bda(), 0x63);  // treat as timeout: keep the bond
    return;
  }
  if (st_ == ST_RUN) {
    const uint32_t now = millis();
    // WATCHDOG 1 - silent link: in RUN there is otherwise NO liveness check, so a
    // link that dies without a DISCONNECT event (zombie link, appliance BLE reset)
    // would freeze polling forever. If nothing has been received for too long,
    // treat the link as dead and reconnect.
    if (now - last_rx_ > RUN_RX_TIMEOUT_MS) {
      force_reconnect_("no BLE response in RUN");
      return;
    }
    // WATCHDOG 2 - stuck TX: tx_busy_ is only cleared when a fragment write is
    // acked (WRITE_CHAR_EVT). If that ack never comes (link went away mid-send),
    // tx_busy_ would latch true and every poll below would early-return forever.
    // Break out of a TX that has been in progress too long.
    if (tx_busy_) {
      if (now - tx_start_ > TX_STUCK_MS) force_reconnect_("TX stuck in RUN");
      return;
    }
    // Pending parameter writes go out first, before the next GetList poll.
    if (!tx_queue_.empty()) {
      auto f = tx_queue_.front();
      tx_queue_.erase(tx_queue_.begin());
      ESP_LOGD(TAG, "sending queued write (%d bytes)", (int) f.size());
      start_tx_(std::move(f));
      return;
    }
    // Individual reads from the previous cycle still draining (recovery if a
    // response was missed; normally they chain off each response, see NOTIFY).
    if (send_next_individual_()) return;
    // Start a fresh cycle: one bulk GetList, then queue the single-read items.
    build_request_();
    tx_off_ = 0; tx_seq_ = 0; tx_busy_ = true; tx_start_ = now;
    ESP_LOGD(TAG, "polling: GetList (%d bytes), responses %u", (int) tx_msg_.size(), (unsigned) responses_);
    queue_individual_reads_();
    send_next_fragment_();
    return;
  }

  // Not yet running: drive the connection sequence from here so we don't depend
  // on catching SEARCH_CMPL at exactly the right instant.
  if (h_1b_ == 0) {
    if (!resolve_handles_()) {
      ESP_LOGD(TAG, "waiting for service discovery (node_state=%d)", (int) this->node_state);
    }
    return;  // resolve_handles_ starts notify registration on success
  }
  if (st_ == ST_INIT) {
    // Handles resolved but the gate never started: notifications not confirmed.
    ESP_LOGW(TAG, "notifications not confirmed (%d/%d) - re-registering", descr_done_, notify_expected_);
    start_notify_();
    return;
  }
  // The gate/CANIP handshake started but did not reach RUN before this tick.
  // The appliance issues only ONE gate challenge per connection, so re-triggering
  // the gate mid-session cannot recover - force a clean reconnect and start over.
  char msg[48];
  snprintf(msg, sizeof(msg), "handshake stalled (st=%d)", (int) st_);
  force_reconnect_(msg);
}

// Close the GATT link and reset state so ESPHome's ble_client reconnects cleanly.
// Used both for a stalled handshake and for the RUN-phase watchdogs. Resetting
// tx_busy_ here is what guarantees a wedged TX can never stay latched.
void RemehaConfida::force_reconnect_(const char *why) {
  ESP_LOGW(TAG, "%s - forcing reconnect", why);
  reconnects_++;
  // Mirror the reason + count to the optional HA diagnostic entities, so it can be
  // reviewed later from HA history without watching the live ESPHome log.
  if (reason_ts_ != nullptr) reason_ts_->publish_state(why);
  if (reconnect_sensor_ != nullptr) reconnect_sensor_->publish_state((float) reconnects_);
  st_ = ST_INIT;
  tx_busy_ = false;
  rx_msg_.clear(); indiv_q_.clear();
  esp_ble_gattc_close(this->parent()->get_gattc_if(), this->parent()->get_conn_id());
}

void RemehaConfida::start_tx_(std::vector<uint8_t> frame) {
  tx_msg_ = std::move(frame);
  tx_off_ = 0; tx_seq_ = 0; tx_busy_ = true; tx_start_ = millis();
  send_next_fragment_();
}

// Build a WRITE frame (operation 0x04) and queue it. No leading 0xFF (see
// build_read_frame_): on air it becomes FF 01 02 00 00 00 04 00 ... Sent
// immediately when the bus is idle and the session is running, otherwise on the
// next poll tick.
void RemehaConfida::enqueue_write(uint8_t node, uint16_t index, uint8_t sub,
                                  const std::vector<uint8_t> &data) {
  std::vector<uint8_t> body = {0x01, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, node,
                               (uint8_t)(index >> 8), (uint8_t)(index & 0xFF), sub};
  body.insert(body.end(), data.begin(), data.end());
  uint16_t c = crc_modbus_(body.data(), body.size());
  std::vector<uint8_t> frame;
  frame.reserve(body.size() + 2);
  frame.insert(frame.end(), body.begin(), body.end());
  frame.push_back((uint8_t)(c >> 8));
  frame.push_back((uint8_t)(c & 0xFF));
  ESP_LOGI(TAG, ">> queue WRITE 0x%04X:%u = %s (frame %s)", index, sub,
           tohex(data.data(), data.size()).c_str(), tohex(frame.data(), frame.size()).c_str());
  tx_queue_.push_back(std::move(frame));
  if (st_ == ST_RUN && !tx_busy_) {
    auto f = tx_queue_.front();
    tx_queue_.erase(tx_queue_.begin());
    start_tx_(std::move(f));
  }
}

void RemehaConfida::send_next_fragment_() {
  if (tx_off_ >= tx_msg_.size()) { tx_busy_ = false; return; }
  size_t remain = tx_msg_.size() - tx_off_;
  size_t chunk = remain > FRAG_DATA ? FRAG_DATA : remain;
  bool last = (tx_off_ + chunk) >= tx_msg_.size();
  uint8_t buf[1 + FRAG_DATA];
  buf[0] = last ? 0xFF : tx_seq_++;
  memcpy(buf + 1, tx_msg_.data() + tx_off_, chunk);
  tx_off_ += chunk;
  write_raw_(h_canip_w_, buf, chunk + 1);
}

// Decode one object's value bytes and publish it to whichever platform(s) the
// matching item is bound to. Shared by the bulk GetList path and the individual
// read path. `flag` 0x02 = "typed" (2-byte value + 2-byte type tag); anything
// else = raw little-endian value in `len` bytes.
void RemehaConfida::decode_and_publish_(RemehaItem &it, uint8_t flag, const uint8_t *data, uint8_t len) {
  if (it.text_sensor != nullptr) {
    std::string s;
    for (uint8_t j = 0; j < len && data[j]; j++) s.push_back((char) data[j]);
    it.text_sensor->publish_state(s);
    return;
  }
  if (it.sensor == nullptr && it.number == nullptr) return;
  int64_t raw = 0; bool ok = true;
  bool is_signed = (it.type == VT_I8 || it.type == VT_I16 || it.type == VT_I32);
  if (flag == 0x02) {
    ok = len >= 2;
    if (ok) {
      uint16_t v = (uint16_t)(data[0] | (data[1] << 8));
      raw = is_signed ? (int16_t) v : (int64_t) v;
    }
  } else {
    switch (it.type) {
      case VT_U8: case VT_ENUM: case VT_BOOL: ok = len >= 1; if (ok) raw = data[0]; break;
      case VT_I8:  ok = len >= 1; if (ok) raw = (int8_t) data[0]; break;
      case VT_U16: ok = len >= 2; if (ok) raw = (uint16_t)(data[0] | data[1] << 8); break;
      case VT_I16: ok = len >= 2; if (ok) raw = (int16_t)(data[0] | data[1] << 8); break;
      case VT_U32: ok = len >= 4; if (ok) raw = (uint32_t)(data[0] | data[1] << 8 | data[2] << 16 | (uint32_t) data[3] << 24); break;
      case VT_I32: ok = len >= 4; if (ok) raw = (int32_t)(data[0] | data[1] << 8 | data[2] << 16 | (uint32_t) data[3] << 24); break;
    }
  }
  // "not available" sentinels for sensors that aren't fitted
  bool na = (raw == -32768 && is_signed) ||
            (raw == 0xFFFF && !is_signed && (flag == 0x02 || it.type == VT_U16)) ||
            (it.type == VT_I32 && flag != 0x02 && raw == -2147483648LL) ||
            (it.type == VT_U32 && flag != 0x02 && raw == 0xFFFFFFFFLL);
  if (!ok || na) return;
  // Boolean status flags (e.g. 0x502E:x) use 0 = no and 0xFF = yes (shown as
  // "Ja"). Map any non-zero value to 1.
  if (it.type == VT_BOOL) raw = raw != 0 ? 1 : 0;
  float val = (float) raw * it.gain;
  ESP_LOGD(TAG, "0x%04X:%u = %.3f", it.index, it.sub, val);
  if (it.sensor != nullptr) it.sensor->publish_state(val);
  if (it.number != nullptr) {
    ESP_LOGI(TAG, "read-back 0x%04X:%u = %.3f (number '%s')", it.index, it.sub, val,
             it.number->get_name().c_str());
    it.number->publish_state(val);  // actual current value
  }
}

void RemehaConfida::parse_response_(const std::vector<uint8_t> &m) {
  if (m.size() < 10) return;
  if (crc_modbus_(m.data(), m.size() - 2) != (uint16_t)((m[m.size() - 2] << 8) | m[m.size() - 1])) {
    ESP_LOGW(TAG, "response CRC mismatch (%u bytes)", (unsigned) m.size());
    return;
  }
  const size_t end = m.size() - 2;
  uint8_t op = m[5];  // 0x09 = bulk GetList response, 0x01 = individual read response, 0x05 = write response

  if (op == 0x01) {
    // Individual read response: <01><00><node><idxHi><idxLo><sub><value...>
    // The whole remaining payload (offset 11 .. end) is the raw LE value.
    if (end < 12) { ESP_LOGW(TAG, "short individual response (%u bytes)", (unsigned) m.size()); return; }
    uint8_t node = m[7];
    uint16_t index = (uint16_t)(m[8] << 8) | m[9];
    uint8_t sub = m[10];
    const uint8_t *data = &m[11];
    uint8_t len = (uint8_t)(end - 11);
    for (auto &it : items_) {
      if (it.node == node && it.index == index && it.sub == sub) {
        decode_and_publish_(it, 0x01, data, len);
        break;
      }
    }
    return;
  }

  if (op == 0x05 && end >= 11) {  // write accepted (no payload)
    uint16_t index = (uint16_t)(m[8] << 8) | m[9];
    char msg[64];
    snprintf(msg, sizeof(msg), "OK: 0x%04X:%u node %u", index, m[10], m[7]);
    ESP_LOGI(TAG, "<< WRITE geaccepteerd 0x%04X:%u (node %u)", index, m[10], m[7]);
    if (write_result_ts_ != nullptr) write_result_ts_->publish_state(msg);
    return;
  }

  if ((op == 0x02 || op == 0x06) && end >= 11) {
    // Error response: <op><00><node><idxHi><idxLo><sub><abort code: 4 bytes LE>
    uint16_t index = (uint16_t)(m[8] << 8) | m[9];
    uint32_t code = (end >= 15) ? le32(&m[11]) : 0;
    const char *what = (op == 0x06) ? "WRITE GEWEIGERD" : "READ MISLUKT";
    ESP_LOGW(TAG, "<< %s 0x%04X:%u (node %u): abort 0x%08lX = %s", what, index, m[10], m[7],
             (unsigned long) code, sdo_abort_str(code));
    if (op == 0x06 && write_result_ts_ != nullptr) {
      char msg[96];
      snprintf(msg, sizeof(msg), "geweigerd 0x%04X:%u: %s (0x%08lX)", index, m[10], sdo_abort_str(code),
               (unsigned long) code);
      write_result_ts_->publish_state(msg);
    }
    return;
  }

  if (op != 0x09) {  // anything else: nothing to publish
    ESP_LOGD(TAG, "response op 0x%02X (%u bytes): %s", op, (unsigned) m.size(), tohex(m.data(), m.size()).c_str());
    return;
  }

  // Bulk GetList response. Each entry: <flag:1><node:1><idxHi:1><idxLo:1><sub:1><status:2><len:1><data:len>
  size_t i = 8;
  while (i + 8 <= end) {
    uint8_t flag = m[i];
    uint8_t node = m[i + 1];
    uint16_t index = (uint16_t)(m[i + 2] << 8) | m[i + 3];
    uint8_t sub = m[i + 4];
    uint16_t status = (uint16_t)(m[i + 5] << 8) | m[i + 6];
    uint8_t len = m[i + 7];
    const uint8_t *data = &m[i + 8];
    if (i + 8 + len > end) break;
    if (status != 0x0000) {  // unavailable entry: log once per object so odd statuses become visible
      ESP_LOGV(TAG, "GetList 0x%04X:%u status 0x%04X", index, sub, status);
      i += 8 + len; continue;
    }
    for (auto &it : items_) {
      if (it.node == node && it.index == index && it.sub == sub) {
        decode_and_publish_(it, flag, data, len);
        break;
      }
    }
    i += 8 + len;
  }
}

// ---- writable number parameter ----
void RemehaNumber::control(float value) {
  if (parent_ == nullptr) {
    ESP_LOGW(TAG, "number has no parent hub");
    return;
  }
  float g = (gain_ == 0.0f) ? 1.0f : gain_;
  long raw = lroundf(value / g);
  std::vector<uint8_t> data;
  switch (type_) {
    case VT_U8: case VT_I8: case VT_ENUM:
      data.push_back((uint8_t)(raw & 0xFF));
      break;
    case VT_BOOL:  // write the appliance's own encoding: 0 = no, 0xFF = yes
      data.push_back(raw != 0 ? 0xFF : 0x00);
      break;
    case VT_U16: case VT_I16:
      data.push_back((uint8_t)(raw & 0xFF));
      data.push_back((uint8_t)((raw >> 8) & 0xFF));
      break;
    case VT_U32: case VT_I32:
      data.push_back((uint8_t)(raw & 0xFF));
      data.push_back((uint8_t)((raw >> 8) & 0xFF));
      data.push_back((uint8_t)((raw >> 16) & 0xFF));
      data.push_back((uint8_t)((raw >> 24) & 0xFF));
      break;
  }
  ESP_LOGI(TAG, "number set %.3f -> raw %ld -> write 0x%04X:%u", value, raw, index_, sub_);
  parent_->enqueue_write(node_, index_, sub_, data);
  this->publish_state(value);  // optimistic; the next poll confirms the read-back
}

}  // namespace remeha_confida
}  // namespace esphome
#endif
