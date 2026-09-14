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
  bool speed_change_failed = false;
  bool speed_change_succeeded = false;
  bool speed_change_fails = false;
  int isr_drain_requests = 0;
  uint8_t isr_drain_bus = 0xFF;
  int begin_count = 0;
  int end_count = 0;
  // The receive ring depth the last begin() was configured with; 0 until then.
  uint16_t rx_ring_depth = 0;
  std::deque<CAN_frame> rx;
};

constexpr uint32_t kTwaiErrSt = TWAI_ERR_ST;
constexpr uint32_t kTwaiBusOffSt = TWAI_BUS_OFF_ST;

// Indexed by Chip.
ChipState g_chips[4];
uint32_t g_native_status = 0;
std::vector<SentFrame> g_sent;

// Puts the FD add-ons on the MCP2515's SPI bus, which is the wiring
// mcp2515_bus_is_exclusive() declines the interrupt drain for. Read by
// EmulCanHal, so a test sets it BEFORE emul_can_init_on_full_board().
bool g_fd_bus_shared = false;

/* The second FD add-on on a controller of its OWN, with its own SCK/SDO/SDI.
 * The T-2CAN in its FD fitment is wired this way (hw_lilygo2can.h puts the
 * second MCP2518 on the bus the MCP2515 would have used), and it is the shape
 * in which a failure of the FIRST FD bus decides nothing for the second chip.
 * Read by EmulCanHal, so a test sets it BEFORE emul_can_init_on_full_board().
 */
bool g_second_fd_own_bus = false;

/* Makes the first FD bus's SCK the pad the MCP2515 has already been given, so
 * alloc_pins() refuses it with EVENT_GPIO_CONFLICT. This is the only way to
 * reach a FAILED shared FD bus: a bus whose pins are merely absent is refused
 * one level earlier by plan_canfd_init(), which leaves fd_bus_ok true. Real
 * boards do declare one pad twice - hw_3LB gives MCP2515_MISO and MCP2517_INT
 * the same number.
 */
bool g_fd_bus_pin_conflict = false;

/* Both FD add-ons are the same class, so an ACAN2517FD takes its identity from
 * the chip select it was handed - the one thing the two blocks in init_CAN() do
 * not share.
 *
 * It used to be construction ORDER, canfd first and canfd_2 second, and that is
 * only equivalent while both are constructed. The second chip can be on its own
 * SPI bus and come up after the first one's bus has failed, in which case the
 * first is never built and the SECOND takes its identity - so a test asserting
 * about "the second FD add-on" would be reading the first one's state, and
 * would say so about a chip that does not exist.
 */
int fd_chip_for_cs(uint8_t cs) {
  const gpio_num_t cs2 = esp32hal != nullptr ? esp32hal->MCP2517_CS2() : GPIO_NUM_NC;
  if (cs2 != GPIO_NUM_NC && cs == static_cast<uint8_t>(cs2)) {
    return static_cast<int>(Chip::Mcp2518fd2);
  }
  return static_cast<int>(Chip::Mcp2518fd);
}

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
  g_fd_bus_shared = false;
  g_second_fd_own_bus = false;
  g_fd_bus_pin_conflict = false;
}

void set_begin_error(Chip chip, uint32_t error_code) {
  state(chip).begin_error = error_code;
}

void set_send_fails(Chip chip, bool fails) {
  state(chip).send_fails = fails;
}

void set_speed_change_fails(Chip chip, bool fails) {
  state(chip).speed_change_fails = fails;
}

void set_fd_bus_shared_with_2515(bool shared) {
  g_fd_bus_shared = shared;
}

void set_second_fd_on_its_own_bus(bool own_bus) {
  g_second_fd_own_bus = own_bus;
}

void set_first_fd_bus_pin_conflict(bool conflict) {
  g_fd_bus_pin_conflict = conflict;
}

int isr_drain_requests(Chip chip) {
  return state(chip).isr_drain_requests;
}

uint8_t isr_drain_bus(Chip chip) {
  return state(chip).isr_drain_bus;
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

uint16_t rx_ring_depth(Chip chip) {
  return state(chip).rx_ring_depth;
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
  s.rx_ring_depth = inSettings.mDriverReceiveBufferSize;
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

uint16_t ACAN_ESP32::driverReceiveBufferSize() const {
  return emul_can::g_chips[static_cast<int>(Chip::Native)].rx_ring_depth;
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

void MCP2515_Lite::useIsrDrain(uint8_t spi_bus) {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Mcp2515)];
  s.isr_drain_requests++;
  s.isr_drain_bus = spi_bus;
}

bool MCP2515_Lite::isrDrainActive() const {
  return false;
}

void MCP2515_Lite::changeSpeed(const MCP2515_Lite_Speed& new_speed) {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Mcp2515)];
  // The two verdicts are set together, never independently: on the real chip
  // enacting a change retires the opposite one, so a stub that only ever sets
  // the failure would let a caller read a stale success from an earlier change.
  s.speed_change_failed = s.speed_change_fails || s.begin_error != 0;
  s.speed_change_succeeded = !s.speed_change_failed;
}

bool MCP2515_Lite::speedChangeFailed() {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Mcp2515)];
  const bool failed = s.speed_change_failed;
  s.speed_change_failed = false;
  return failed;
}

bool MCP2515_Lite::speedChangeSucceeded() {
  auto& s = emul_can::g_chips[static_cast<int>(Chip::Mcp2515)];
  const bool ok = s.speed_change_succeeded;
  s.speed_change_succeeded = false;
  return ok;
}

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
    : chip_(emul_can::fd_chip_for_cs(inCS)), cs_(inCS), int_(inINT) {}

ACAN2517FD::~ACAN2517FD() {}

uint32_t ACAN2517FD::begin(const ACAN2517FDSettings& inSettings, void (*inInterruptServiceRoutine)(void)) {
  auto& s = emul_can::g_chips[chip_];
  s.begin_count++;
  s.rx_ring_depth = inSettings.mDriverReceiveFIFOSize;
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
  // ALL FIVE, which is what "the all-interfaces HAL" means and what every pin
  // declaration below already describes. init_CAN() consults this list first and
  // erases anything the board does not declare, so a HAL that names three while
  // routing five silently removes the two it forgot - and a test asserting that
  // every requested interface came up then fails on a refusal it never asked
  // for, pointing at the chip rather than at this list.
  std::vector<comm_interface> available_interfaces() override {
    return {comm_interface::CanNative, comm_interface::CanFdNative, comm_interface::CanAddonMcp2515,
            comm_interface::CanFdAddonMcp2518, comm_interface::CanFdAddonMcp2518_2};
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

  uint8_t MCP2517_BUS() override { return emul_can::g_fd_bus_shared ? VSPI : HSPI; }
  // The MCP2515's SCK when a test asks for a conflict: alloc_pins() records the
  // 2515's pads first, so this one is refused where the bus is brought up.
  gpio_num_t MCP2517_SCK() override { return emul_can::g_fd_bus_pin_conflict ? GPIO_NUM_22 : GPIO_NUM_12; }
  gpio_num_t MCP2517_SDI() override { return GPIO_NUM_13; }
  gpio_num_t MCP2517_SDO() override { return GPIO_NUM_14; }
  gpio_num_t MCP2517_CS() override { return GPIO_NUM_15; }
  gpio_num_t MCP2517_INT() override { return GPIO_NUM_16; }
  uint32_t MCP2517_FREQ() override { return 40000000; }

  // Second FD add-on on the same SPI bus, which is how the boards that carry two
  // of them are wired - only CS and INT are its own.
  uint8_t MCP2517_BUS2() override {
    if (emul_can::g_second_fd_own_bus) {
      // A controller the first FD chip is not on, whichever one that is.
      return emul_can::g_fd_bus_shared ? HSPI : VSPI;
    }
    return emul_can::g_fd_bus_shared ? VSPI : HSPI;
  }
  gpio_num_t MCP2517_CS2() override { return GPIO_NUM_25; }
  gpio_num_t MCP2517_INT2() override { return GPIO_NUM_26; }
  // Routed only in the own-bus wiring; NC otherwise, which is what "this chip
  // shares the first bus" means to plan_canfd_init().
  gpio_num_t MCP2517_SCK2() override { return emul_can::g_second_fd_own_bus ? GPIO_NUM_27 : GPIO_NUM_NC; }
  gpio_num_t MCP2517_SDI2() override { return emul_can::g_second_fd_own_bus ? GPIO_NUM_32 : GPIO_NUM_NC; }
  gpio_num_t MCP2517_SDO2() override { return emul_can::g_second_fd_own_bus ? GPIO_NUM_33 : GPIO_NUM_NC; }
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
}

void emul_can_init_on_full_board() {
  // The board HAL is swapped out only for the duration of init_CAN(), so that
  // the pins it allocates land in a HAL nothing else will see and the caller
  // keeps whatever board it had selected.
  Esp32Hal* const board = esp32hal;
  esp32hal = new EmulCanHal();

  init_CAN();

  delete esp32hal;
  esp32hal = board;
}

void emul_can_bring_up_all_interfaces() {
  emul_can_tear_down_all_interfaces();

  register_can_receiver(&g_null_receiver, CAN_NATIVE);
  register_can_receiver(&g_null_receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&g_null_receiver, CANFD_ADDON_MCP2518);
  register_can_receiver(&g_null_receiver, CANFD_ADDON_MCP2518_2);

  emul_can_init_on_full_board();
}
