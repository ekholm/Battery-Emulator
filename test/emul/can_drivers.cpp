#include "can_drivers.h"

#include <deque>

#include "src/devboard/hal/hal.h"
#include "src/lib/mcp2515_lite/mcp2515_lite.h"
#include "src/lib/pierremolinaro-ACAN2517FD/ACAN2517FD.h"
#include "src/lib/pierremolinaro-acan-esp32/ACAN_ESP32.h"

#include "../../Software/src/communication/can/CanReceiver.h"
#include "../../Software/src/communication/can/comm_can.h"

#include <esp_private/periph_ctrl.h>

// Fed for every frame a chip accepts; see emul/can.cpp.
extern std::vector<CAN_frame> g_emul_transmitted_frames;

namespace emul_can {
namespace {

struct ChipState {
  uint32_t begin_error = 0;
  bool send_fails = false;
  bool has_error = false;
  bool running = false;
  bool paused = false;
  int begin_count = 0;
  int end_count = 0;
  std::deque<CAN_frame> rx;
};

constexpr uint32_t kTwaiErrSt = TWAI_ERR_ST;
constexpr uint32_t kTwaiBusOffSt = TWAI_BUS_OFF_ST;

// Indexed by Chip.
ChipState g_chips[4];
uint32_t g_native_status = 0;
std::vector<SentFrame> g_sent;

// Both FD add-ons are the same class, so an ACAN2517FD takes its identity from
// the order comm_can.cpp constructs them in: canfd first, canfd_2 second.
int g_next_fd_chip = 0;

ChipState& state(Chip chip) {
  return g_chips[static_cast<int>(chip)];
}

bool send(Chip chip, const CAN_frame& frame) {
  ChipState& s = state(chip);
  if (s.send_fails) {
    return false;
  }
  g_sent.push_back({chip, frame});
  g_emul_transmitted_frames.push_back(frame);
  return true;
}

bool take_error(Chip chip) {
  ChipState& s = state(chip);
  const bool had = s.has_error;
  s.has_error = false;
  return had;
}

}  // namespace

void reset() {
  for (ChipState& s : g_chips) {
    s = ChipState();
  }
  g_native_status = 0;
  g_sent.clear();
  g_emul_transmitted_frames.clear();
  g_next_fd_chip = 0;
}

void set_begin_error(Chip chip, uint32_t error_code) {
  state(chip).begin_error = error_code;
}

void set_send_fails(Chip chip, bool fails) {
  state(chip).send_fails = fails;
}

void set_bus_error(Chip chip, bool has_error) {
  state(chip).has_error = has_error;
  if (chip == Chip::Native) {
    g_native_status = has_error ? (g_native_status | kTwaiErrSt) : (g_native_status & ~kTwaiErrSt);
  }
}

void set_native_bus_off(bool bus_off) {
  g_native_status = bus_off ? (g_native_status | kTwaiBusOffSt) : (g_native_status & ~kTwaiBusOffSt);
}

void queue_received(Chip chip, const CAN_frame& frame) {
  state(chip).rx.push_back(frame);
}

const std::vector<SentFrame>& sent_frames() {
  return g_sent;
}

int begin_count(Chip chip) {
  return state(chip).begin_count;
}

int end_count(Chip chip) {
  return state(chip).end_count;
}

bool is_running(Chip chip) {
  return state(chip).running;
}

bool is_paused(Chip chip) {
  return state(chip).paused;
}

}  // namespace emul_can

using emul_can::Chip;

// ---------------------------------------------------------------------------
// Native CAN (ACAN_ESP32)
// ---------------------------------------------------------------------------

ACAN_ESP32 ACAN_ESP32::can;

uint32_t ACAN_ESP32::begin(const ACAN_ESP32_Settings& inSettings) {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Native)];
  s.begin_count++;
  if (s.begin_error != 0) {
    s.running = false;
    return s.begin_error;
  }
  s.running = true;
  return 0;
}

void ACAN_ESP32::end() {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Native)];
  s.end_count++;
  s.running = false;
}

bool ACAN_ESP32::available() const {
  return !emul_can::g_chips[static_cast<int>(Chip::Native)].rx.empty();
}

bool ACAN_ESP32::receive(CANMessage& outMessage) {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Native)];
  if (s.rx.empty()) {
    return false;
  }
  const CAN_frame frame = s.rx.front();
  s.rx.pop_front();
  outMessage.id = frame.ID;
  outMessage.ext = frame.ext_ID;
  outMessage.len = frame.DLC;
  for (uint8_t i = 0; i < frame.DLC && i < sizeof(outMessage.data); i++) {
    outMessage.data[i] = frame.data.u8[i];
  }
  return true;
}

bool ACAN_ESP32::tryToSend(const CANMessage& inMessage) {
  CAN_frame frame = {};
  frame.ID = inMessage.id;
  frame.ext_ID = inMessage.ext;
  frame.DLC = inMessage.len;
  for (uint8_t i = 0; i < inMessage.len && i < sizeof(inMessage.data); i++) {
    frame.data.u8[i] = inMessage.data[i];
  }
  return emul_can::send(Chip::Native, frame);
}

uint32_t ACAN_ESP32::statusRegister() const {
  return emul_can::g_native_status;
}

void periph_module_reset(periph_module_t periph) {}

// ---------------------------------------------------------------------------
// Add-on classic CAN (MCP2515_Lite)
// ---------------------------------------------------------------------------

MCP2515_Lite::MCP2515_Lite(SPIClass& spi, uint8_t cs, uint8_t int_pin) : _spi(spi), _cs(cs), _int_pin(int_pin) {}

MCP2515_Lite::~MCP2515_Lite() {}

uint32_t MCP2515_Lite::autodetectOscillatorFrequency() {
  return 8000000;
}

bool MCP2515_Lite::begin(const MCP2515_Lite_Speed& speed, bool loopback, bool skip_task_start) {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Mcp2515)];
  s.begin_count++;
  if (s.begin_error != 0) {
    s.running = false;
    return false;
  }
  s.running = true;
  s.paused = false;
  return true;
}

bool MCP2515_Lite::sendFrame(const MCP2515_Lite_Frame& msg) {
  CAN_frame frame = {};
  frame.ID = msg.id;
  frame.ext_ID = msg.ext;
  frame.DLC = msg.dlc;
  for (uint8_t i = 0; i < msg.dlc && i < sizeof(msg.data); i++) {
    frame.data.u8[i] = msg.data[i];
  }
  return emul_can::send(Chip::Mcp2515, frame);
}

bool MCP2515_Lite::receiveFrame(MCP2515_Lite_Frame& msg) {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Mcp2515)];
  if (s.rx.empty()) {
    return false;
  }
  const CAN_frame frame = s.rx.front();
  s.rx.pop_front();
  msg.fd = false;
  msg.ext = frame.ext_ID;
  msg.dlc = frame.DLC;
  msg.id = frame.ID;
  for (uint8_t i = 0; i < frame.DLC && i < sizeof(msg.data); i++) {
    msg.data[i] = frame.data.u8[i];
  }
  return true;
}

void MCP2515_Lite::changeSpeed(const MCP2515_Lite_Speed& new_speed) {}

void MCP2515_Lite::pause(bool paused) {
  emul_can::g_chips[static_cast<int>(Chip::Mcp2515)].paused = paused;
}

bool MCP2515_Lite::hasErrors() {
  return emul_can::take_error(Chip::Mcp2515);
}

// ---------------------------------------------------------------------------
// Add-on CAN FD (ACAN2517FD), one class behind both FD add-ons
// ---------------------------------------------------------------------------

ACAN2517FD::ACAN2517FD(const uint8_t inCS, SPIClass& inSPI, const uint8_t inINT)
    : chip_(static_cast<int>(emul_can::g_next_fd_chip == 0 ? Chip::Mcp2518fd : Chip::Mcp2518fd2)),
      cs_(inCS),
      int_(inINT) {
  emul_can::g_next_fd_chip++;
}

ACAN2517FD::~ACAN2517FD() {}

uint32_t ACAN2517FD::begin(const ACAN2517FDSettings& inSettings, void (*inInterruptServiceRoutine)(void)) {
  auto& s = emul_can::g_chips[chip_];
  s.begin_count++;
  if (s.begin_error != 0) {
    s.running = false;
    return s.begin_error;
  }
  s.running = true;
  return 0;
}

bool ACAN2517FD::end() {
  auto& s = emul_can::g_chips[chip_];
  s.end_count++;
  s.running = false;
  return true;
}

bool ACAN2517FD::tryToSend(const CANFDMessage& inMessage) {
  CAN_frame frame = {};
  frame.FD = (inMessage.type == CANFDMessage::CANFD_NO_BIT_RATE_SWITCH ||
              inMessage.type == CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH);
  frame.ID = inMessage.id;
  frame.ext_ID = inMessage.ext;
  frame.DLC = inMessage.len;
  for (uint8_t i = 0; i < inMessage.len && i < sizeof(frame.data.u8); i++) {
    frame.data.u8[i] = inMessage.data[i];
  }
  return emul_can::send(static_cast<Chip>(chip_), frame);
}

bool ACAN2517FD::receive(CANFDMessage& outMessage) {
  auto& s = emul_can::g_chips[chip_];
  if (s.rx.empty()) {
    return false;
  }
  const CAN_frame frame = s.rx.front();
  s.rx.pop_front();
  outMessage.type = frame.FD ? CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH : CANFDMessage::CAN_DATA;
  outMessage.id = frame.ID;
  outMessage.ext = frame.ext_ID;
  outMessage.len = frame.DLC;
  for (uint8_t i = 0; i < frame.DLC && i < sizeof(frame.data.u8); i++) {
    outMessage.data[i] = frame.data.u8[i];
  }
  return true;
}

bool ACAN2517FD::available() {
  return !emul_can::g_chips[chip_].rx.empty();
}

bool ACAN2517FD::hasCanErrors() {
  return emul_can::take_error(static_cast<Chip>(chip_));
}

void ACAN2517FD::poll() {}

void ACAN2517FD::isr() {}

// ---------------------------------------------------------------------------
// Bringing the interfaces up and down through the real init_CAN()
// ---------------------------------------------------------------------------

namespace {

// A board that has every CAN interface fitted, so that one init_CAN() can bring
// all four up. No real board does; the point here is to exercise every branch of
// init_CAN() from one place, and a test that wants a board WITHOUT some chip
// registers no receiver for it.
class EmulCanHal : public Esp32Hal {
 public:
  const char* name() override { return "Emulated CAN board"; }
  std::vector<comm_interface> available_interfaces() override {
    return {comm_interface::CanNative, comm_interface::CanAddonMcp2515, comm_interface::CanFdAddonMcp2518};
  }

  gpio_num_t CAN_TX_PIN() override { return GPIO_NUM_5; }
  gpio_num_t CAN_RX_PIN() override { return GPIO_NUM_4; }

  uint8_t MCP2515_BUS() override { return VSPI; }
  gpio_num_t MCP2515_SCK() override { return GPIO_NUM_22; }
  gpio_num_t MCP2515_MOSI() override { return GPIO_NUM_21; }
  gpio_num_t MCP2515_MISO() override { return GPIO_NUM_19; }
  gpio_num_t MCP2515_CS() override { return GPIO_NUM_18; }
  gpio_num_t MCP2515_INT() override { return GPIO_NUM_23; }
  uint32_t MCP2515_FREQ() override { return 8000000; }

  uint8_t MCP2517_BUS() override { return HSPI; }
  gpio_num_t MCP2517_SCK() override { return GPIO_NUM_12; }
  gpio_num_t MCP2517_SDI() override { return GPIO_NUM_13; }
  gpio_num_t MCP2517_SDO() override { return GPIO_NUM_14; }
  gpio_num_t MCP2517_CS() override { return GPIO_NUM_15; }
  gpio_num_t MCP2517_INT() override { return GPIO_NUM_16; }
  uint32_t MCP2517_FREQ() override { return 40000000; }

  // Second FD add-on on the same SPI bus, which is how the boards that carry two
  // of them are wired - only CS and INT are its own.
  uint8_t MCP2517_BUS2() override { return HSPI; }
  gpio_num_t MCP2517_CS2() override { return GPIO_NUM_25; }
  gpio_num_t MCP2517_INT2() override { return GPIO_NUM_26; }
  uint32_t MCP2517_FREQ2() override { return 40000000; }
};

// A receiver that only exists so that an interface counts as requested. Frames
// it is handed are dropped; tests that care about dispatch register their own.
class NullCanReceiver : public CanReceiver {
 public:
  void receive_can_frame(CAN_frame* rx_frame) override {}
};

NullCanReceiver g_null_receiver;

}  // namespace

void emul_can_tear_down_all_interfaces() {
  comm_can_reset_for_test();
  emul_can::g_next_fd_chip = 0;
}

bool emul_can_init_on_full_board() {
  // The board HAL is swapped out only for the duration of init_CAN(), so that
  // the pins it allocates land in a HAL nothing else will see and the caller
  // keeps whatever board it had selected.
  Esp32Hal* const board = esp32hal;
  esp32hal = new EmulCanHal();

  const bool ok = init_CAN();

  delete esp32hal;
  esp32hal = board;
  return ok;
}

void emul_can_bring_up_all_interfaces() {
  emul_can_tear_down_all_interfaces();

  register_can_receiver(&g_null_receiver, CAN_NATIVE);
  register_can_receiver(&g_null_receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&g_null_receiver, CANFD_ADDON_MCP2518);
  register_can_receiver(&g_null_receiver, CANFD_ADDON_MCP2518_2);

  emul_can_init_on_full_board();
}
