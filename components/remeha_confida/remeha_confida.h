// ============================================================================
//  Remeha Confida -> ESPHome BLE component  (declarations)
// ----------------------------------------------------------------------------
//  RemehaConfida is both a PollingComponent (update() every 30 s) and a
//  BLEClientNode (receives GATT events). It owns a list of RemehaItem, each
//  the binding between one appliance object (node/index/subindex) and one HA
//  entity (sensor / text_sensor / number). The full protocol is documented at
//  the top of remeha_confida.cpp; this header just declares the moving parts.
// ============================================================================
#pragma once
#ifdef USE_ESP32
#include <vector>
#include <set>
#include <string>
#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/number/number.h"
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>

namespace esphome {
namespace remeha_confida {

namespace espbt = esphome::esp32_ble_tracker;

// How a value's raw bytes are interpreted. VT_ENUM is an 8-bit enumeration
// (status/on-off/option); the rest are little-endian integers of that width.
// VT_BOOL is an 8-bit flag stored as 0 / 0xFF (published as 0 / 1).
enum ValueType { VT_U8, VT_I8, VT_U16, VT_I16, VT_U32, VT_I32, VT_ENUM, VT_BOOL };

// One appliance object <-> one HA entity. Exactly one of sensor/text_sensor is
// set for a read-only value; a writable parameter sets `number` (and is polled
// so the number shows the real read-back). The raw value is decoded as `type`
// and multiplied by `gain` (e.g. 0.01 for centidegrees).
struct RemehaItem {
  uint8_t node;                      // CAN node: 1 = EHC-16 heat-pump controller (default)
  uint16_t index;                    // CANopen object index (e.g. 0x4301 = flow temp)
  uint8_t sub;                       // subindex: 0 for scalars, >=1 for array objects (see .cpp)
  ValueType type;                    // how to decode the raw bytes
  float gain;                        // scale factor applied to the raw integer
  sensor::Sensor *sensor{nullptr};        // numeric read-only value
  text_sensor::TextSensor *text_sensor{nullptr};  // string value (e.g. device name)
  number::Number *number{nullptr};   // writable param: read-back is published here too
  bool single{false};                // true = read via individual request (op 0x00), not bulk GetList
};

// connection state machine
enum St { ST_INIT = 0, ST_GATE_WAIT_CHAL, ST_GATE_WAIT_ACK,
          ST_CANIP_WAIT_CHAL, ST_CANIP_WAIT_ACK, ST_CANIP_WAIT_RX6, ST_RUN };

class RemehaConfida : public PollingComponent,
                      public ble_client::BLEClientNode,
                      public esp32_ble_tracker::ESPBTDeviceListener {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_service_uuid(const std::string &u) { service_uuid_ = espbt::ESPBTUUID::from_raw(u); }
  void set_write_uuid(const std::string &u)   { write_uuid_   = espbt::ESPBTUUID::from_raw(u); }
  void set_notify_uuid(const std::string &u)  { notify_uuid_  = espbt::ESPBTUUID::from_raw(u); }
  void set_passkey(uint32_t pk) { passkey_ = pk; has_passkey_ = true; }
  // Auto-connect by name: if the ble_client MAC is left as 00:00:00:00:00:00,
  // the hub scans and connects to the first device whose advertised name starts
  // with this prefix (default "EHC-"). With a real MAC filled in, scanning is
  // skipped and that MAC is used directly.
  void set_name_prefix(const std::string &p) { name_prefix_ = p; }
  void add_item(const RemehaItem &it) { items_.push_back(it); }
  // Optional diagnostic entities (hub-level): last reconnect reason + reconnect count.
  void set_reason_sensor(text_sensor::TextSensor *t) { reason_ts_ = t; }
  void set_reconnect_sensor(sensor::Sensor *s) { reconnect_sensor_ = s; }
  // Optional: result of the last parameter write (accepted / refused + reason).
  void set_write_result_sensor(text_sensor::TextSensor *t) { write_result_ts_ = t; }
  // Enable/disable reading. false = release the BLE link and STOP reconnecting, so
  // a phone or service tool can connect; true = resume. Driven by a switch.
  void set_active(bool a);

  // Queue a parameter WRITE (operation 0x04). data = value bytes, LITTLE-endian.
  // Sent as soon as the bus is idle; the appliance replies with a write-response.
  void enqueue_write(uint8_t node, uint16_t index, uint8_t sub, const std::vector<uint8_t> &data);

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

  // esp32_ble_tracker listener: sees every advertisement during scanning. Used
  // only in auto-connect mode (MAC left at 0) to discover the appliance by name
  // and to log an overview of all BLE devices seen, so a user with several
  // appliances can read the right MAC from the log and pin it in the YAML.
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;

 protected:
  // CANIP service (0x0e write, 0x10 notify) — configurable
  espbt::ESPBTUUID service_uuid_, write_uuid_, notify_uuid_;
  // gate service + its 4 characteristics (fixed UUIDs for the Confida BLE module)
  espbt::ESPBTUUID gate_svc_, gate_1b_, gate_1d_, gate_20_, gate_22_;
  // second CANIP notify characteristic that is also subscribed to (fixed UUID)
  espbt::ESPBTUUID canip_extra_;

  uint32_t passkey_{0};
  bool has_passkey_{false};

  // resolved value handles
  uint16_t h_canip_w_{0}, h_canip_n_{0};
  uint16_t h_1b_{0}, h_1d_{0}, h_20_{0}, h_22_{0};
  uint16_t h_13_{0};  // extra CANIP notify char that is also subscribed to
  uint8_t reg_done_{0}, descr_done_{0}, notify_expected_{0};
  bool dumped_{false};
  uint16_t cccd_1d_{0}, cccd_22_{0}, cccd_canip_{0}, cccd_13_{0};
  St st_{ST_INIT};
  uint8_t auth_level_{4};

  std::vector<RemehaItem> items_;
  std::vector<std::vector<uint8_t>> tx_queue_;  // pending WRITE frames (sent before polling)
  std::vector<std::vector<uint8_t>> indiv_q_;   // pending individual-READ frames (op 0x00) for single-read items
  std::vector<uint8_t> tx_msg_;   // GetList request / write frame being fragmented
  size_t tx_off_{0};
  uint8_t tx_seq_{0};
  bool tx_busy_{false};
  uint32_t tx_start_{0};          // millis() when the current TX frame began (stuck-TX watchdog)
  std::vector<uint8_t> rx_msg_;   // CANIP notify reassembly
  uint32_t responses_{0};
  uint32_t last_rx_{0};           // millis() of last reassembled response (RUN liveness watchdog)
  uint32_t reconnects_{0};        // total forced reconnects since boot (diagnostic)
  bool connected_once_{false};    // so "verbonden" is published on the first RUN entry
  bool active_{true};             // false = released (link dropped, reconnect suppressed)
  // --- auto-connect by name (only when the configured MAC is 0) ---
  std::string name_prefix_{"EHC-"};  // advertised-name prefix of the appliance
  bool scan_mode_{false};            // true = MAC was 0, discover by name
  bool addr_resolved_{false};        // a matching device was found + address set
  std::set<uint64_t> seen_;          // MACs already logged (log each once)
  // --- pairing guard (see "PAIRING RULES" in the .cpp) ---
  bool pairing_{false};           // encryption/pairing requested, no AUTH_CMPL yet
  bool auth_handled_{false};      // AUTH_CMPL reaches us twice; act only once per connection
  bool gate_pending_{false};      // notifications ready, gate waits for pairing to finish
  uint32_t pair_start_{0};        // millis() when pairing was requested (never cut it off early)
  uint8_t pair_fails_{0};         // consecutive failed pairings (drives the wait length)
  bool backoff_{false};           // waiting after a failed pairing; ble_client disabled
  uint32_t backoff_until_{0};     // millis() when the next attempt is allowed
  uint32_t last_fail_{0};         // millis() of the last failed pairing
  bool ever_failed_{false};       // last_fail_ is valid
  text_sensor::TextSensor *reason_ts_{nullptr};   // optional: last reconnect reason
  sensor::Sensor *reconnect_sensor_{nullptr};     // optional: reconnect count
  text_sensor::TextSensor *write_result_ts_{nullptr};  // optional: last write result

  static uint16_t crc_modbus_(const uint8_t *d, size_t n);
  static void tea_(uint8_t level, uint32_t serial, uint32_t chal, uint32_t &o1, uint32_t &o2);
  void write_raw_(uint16_t handle, const uint8_t *d, size_t n);
  bool resolve_handles_();
  void dump_gatt_();
  void start_notify_();
  void gate_trigger_();
  void canip_start_();
  void canip_handshake_(const std::vector<uint8_t> &m);
  void build_request_();
  std::vector<uint8_t> build_read_frame_(uint8_t node, uint16_t index, uint8_t sub);  // individual read (op 0x00)
  void queue_individual_reads_();         // (re)fill indiv_q_ with one read frame per single-read item
  bool send_next_individual_();           // pop+send one queued individual read; false if none
  void start_tx_(std::vector<uint8_t> frame);
  void force_reconnect_(const char *why);  // close link + reset state so ble_client reconnects
  void pairing_failed_(const uint8_t *bda, uint8_t reason);  // drop link, maybe clear stale bond, wait
  void publish_reason_(const char *why);
  void send_next_fragment_();
  void parse_response_(const std::vector<uint8_t> &m);
  void decode_and_publish_(RemehaItem &it, uint8_t flag, const uint8_t *data, uint8_t len);  // shared decode for bulk + individual
};

// Writable parameter exposed as a Home Assistant number. On change it builds a
// write frame (operation 0x04) and hands it to the hub's write queue.
class RemehaNumber : public number::Number, public Component {
 public:
  void set_parent(RemehaConfida *p) { parent_ = p; }
  void set_node(uint8_t v) { node_ = v; }
  void set_index(uint16_t v) { index_ = v; }
  void set_sub(uint8_t v) { sub_ = v; }
  void set_value_type(ValueType v) { type_ = v; }
  void set_gain(float v) { gain_ = v; }

 protected:
  void control(float value) override;
  RemehaConfida *parent_{nullptr};
  uint8_t node_{1};
  uint16_t index_{0};
  uint8_t sub_{0};
  ValueType type_{VT_U16};
  float gain_{1.0f};
};

}  // namespace remeha_confida
}  // namespace esphome
#endif
