#ifndef MG_5_BATTERY_H
#define MG_5_BATTERY_H

#include "../datalayer/datalayer.h"
#include "UdsCanBattery.h"

#ifndef SMALL_FLASH_DEVICE

class Mg5Battery : public UdsCanBattery {
 public:
  Mg5Battery() { dtc = &datalayer.battery.dtc; }

  virtual void setup(void);
  virtual void handle_incoming_can_frame(CAN_frame rx_frame);
  virtual void update_values();
  virtual void update_soc(uint16_t soc_times_ten);
  virtual void transmit_can(unsigned long currentMillis);
  static constexpr const char* Name = "MG 5 battery";
  void buildMG5_8AFrame();
  bool supports_contactor_close() { return true; }
  void request_open_contactors() { userRequestContactorClose = false; }
  void request_close_contactors() { userRequestContactorClose = true; }

 protected:
  // The pre-superclass readout requested every status (19 02 FF).
  uint8_t dtc_status_mask() override { return 0xFF; }
  uint16_t handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length) override;
  void on_uds_sequence_step(uint16_t state, uint8_t sid, const uint8_t* data, uint16_t len) override;

 private:
  // Subclass sequence states (must not set the UDS_STATE_INTERNAL bit).
  enum Mg5UdsState : uint16_t {
    MG5_STATE_SESSION_START = 1,  // Enter the extended diagnostic session
    MG5_STATE_SESSION_DONE,
  };
  static const int MAX_PACK_VOLTAGE_DV = 4040;  //5000 = 500.0V
  static const int MIN_PACK_VOLTAGE_DV = 3100;
  static const int MAX_CELL_DEVIATION_MV = 150;
  static const int MAX_CELL_VOLTAGE_MV = 4250;  //Battery is put into emergency stop if one cell goes over this value
  static const int MIN_CELL_VOLTAGE_MV = 2700;  //Battery is put into emergency stop if one cell goes below this value
  static const int TOTAL_BATTERY_CAPACITY_WH = 52500;  // 52.5 kWh

  unsigned long previousMillis10 = 0;   // will store last time a 10ms CAN Message was send
  unsigned long previousMillis20 = 0;   // will store last time a 20ms CAN Message was send
  unsigned long previousMillis100 = 0;  // will store last time a 100ms CAN Message was send
  unsigned long previousMillis200 = 0;
  unsigned long previousMillis1000 = 0;
  unsigned long previousMillis2000 = 0;
  unsigned long previousMillisDTC = 0;
  unsigned long previousMillisPID = 0;

  // For calculating charge and discharge power
  float RealVoltage;
  float RealSoC;
  float tempfloat;

  uint16_t soc = 0;
  uint16_t cell_id = 0;
  uint16_t cellVoltageValidTime = 0;
  static const uint8_t CELL_VOLTAGE_TIMEOUT = 10;  // in seconds

  uint8_t transmitIndex = 0;  //For polling switchcase
  uint8_t previousState = 0;
  uint8_t eightAcycle = 0;
  uint8_t warmupCounter = 0;

  const int MaxChargePower = 11000;  // Maximum allowable charge power for the battery cells, excluding the taper,
  const int StartChargeTaper = 90;   // Battery percentage above which the charge power will taper to zero
  const float ChargeTaperExponent =
      1;  // Shape of charge power taper to zero. 1 is linear. >1 reduces quickly and is small at nearly full.
  const int TricklePower = 20;  // Minimimum trickle charge or discharge power (W)

  const int MaxDischargePower = 11000;     // Maximum allowable discharge power, excluding the taper
  const int MinSoC = 10;                   // Minimum SoC allowed
  const int StartDischargeTaper = 10;      // Battery percentage below which the discharge power will taper to zero
  const float DischargeTaperExponent = 1;  // Shape of discharge power taper to zero. 1 is linear. >1 red

  // rolling counter for 0x8A (0x10..0x1F pattern)
  uint8_t mg5_8a_counter = 0x10;

  // simple toggle to alternate 0x80/0x00 and 0x7F/0xFF (alive / redundancy style)
  bool mg5_8a_flip = false;

  bool userRequestContactorClose = true;
  bool contactorClosed = false;
  bool dtc_clear_wanted = false;   // Contactor close wants the stored DTCs erased
  // The BMS answers diagnostics only inside the extended session; when the
  // diag side goes quiet this long, (re-)enter it - the pre-superclass code
  // did the same on every transaction timeout.
  static const unsigned long SESSION_RETRY_MS = 3000;
  unsigned long last_uds_activity_ms = 0;

  // One-byte-prefixed MG DIDs, scanned in order by the UDS superclass (the
  // same 15 the hand-rolled round-robin requested, same order).
  static constexpr uint16_t MG5_PID_SCAN[15] = {0xB041, 0xB042, 0xB043, 0xB045, 0xB046, 0xB047, 0xB048, 0xB049,
                                                0xB04A, 0xB052, 0xB056, 0xB058, 0xB059, 0xB05C, 0xB061};

  CAN_frame MG5_8A = {.FD = false,
                      .ext_ID = false,
                      .DLC = 8,
                      .ID = 0x08A,
                      .data = {0x80, 0x00, 0x00, 0x04, 0x00, 0x02, 0xBB, 0x3F}};

  CAN_frame MG5_1F1 = {.FD = false,
                       .ext_ID = false,
                       .DLC = 8,
                       .ID = 0x1F1,
                       .data = {0x0E, 0x00, 0x00, 0x00, 0x08, 0x72, 0x00, 0x00}};

  //compute checksum for MG5 0x8A message
  uint8_t computeMG5_8AChecksum(const uint8_t* bytes7) const {
    uint8_t crc = 0;
    for (int i = 0; i < 7; ++i) {
      crc += bytes7[i];
    }
    return crc;
  }
};

#endif

#endif
