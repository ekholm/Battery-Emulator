#include "comm_can.h"
// The three CAN drivers are included through the src_dir root rather than by
// relative path, so that the host test build can put emulated stand-ins in
// front of them - a quoted relative include resolves against this file's own
// directory and cannot be redirected. Everything else here already does this.
#include "CanReceiver.h"
#include "can_init_plan.h"
#include "can_rx_ring_depth.h"
#include "canfd_init_error.h"
#include "comm_can.h"
#include "src/datalayer/datalayer.h"
#include "src/devboard/hal/hal.h"
#include "src/devboard/safety/safety.h"
#include "src/devboard/sdcard/sdcard.h"
#include "src/devboard/utils/events.h"
#include "src/devboard/utils/logging.h"
#include "src/devboard/webserver/webserver_can_streaming.h"
#include "src/lib/mcp2515_lite/mcp2515_lite.h"
#include "src/lib/pierremolinaro-ACAN2517FD/ACAN2517FD.h"
#include "src/lib/pierremolinaro-acan-esp32/ACAN_ESP32.h"
#include "utils.h"

#include <esp_private/periph_ctrl.h>

#include <algorithm>
#include <map>

#ifndef DISABLEMCP2517FDCOMPAT
#error \
    "MCP2517FD compatibility mode turns the library's turnOffInterrupts()/turnOnInterrupts() into a real interrupt mask, and this file calls ACAN2517FD::end(), whose body deletes the driver task between them. end() is not the only site: the same pair brackets the library's transmit, receive, register-access and reset paths."
#endif

/* What this file assumes about the three drivers, checked by the compiler in
 * both builds - against the vendored headers when the firmware is built, and
 * against test/emul/src/lib/... when the host suite is.
 *
 * The two payload lengths are load-bearing twice over: the FD-length refusals
 * on the classic paths below are written as `DLC > sizeof(...::data)`, so a
 * wrong size here does not fail to compile, it silently changes which frames
 * get refused - and the host suite would agree with itself while testing a
 * different rule from the one that ships.
 */
static_assert(sizeof(CANMessage::data) == 8,
              "classic CAN carries 8 bytes - the FD-length refusal is written against this");
static_assert(sizeof(CANFDMessage::data) == 64, "CAN FD carries 64 bytes");
static_assert(sizeof(MCP2515_Lite_Frame::data) == 8, "the MCP2515 is a classic controller");
/* hal.h hands MCP2517_CLKODIV() to mCLKOPin as a raw integer (0b11 for the
 * divide-by-10 default, 0b00 on BECom), so the enumerator ordinals are part of
 * that contract and not an implementation detail of the driver header. */
static_assert(static_cast<int>(ACAN2517FDSettings::CLKO_DIVIDED_BY_1) == 0b00,
              "hal.h's MCP2517_CLKODIV() encodes this ordinal");
static_assert(static_cast<int>(ACAN2517FDSettings::CLKO_DIVIDED_BY_10) == 0b11,
              "hal.h's MCP2517_CLKODIV() default encodes this ordinal");

volatile CAN_Configuration can_config = {.battery = CAN_NATIVE,
                                         .inverter = CAN_NATIVE,
                                         .battery_double = CAN_ADDON_MCP2515,
                                         .battery_triple = CAN_ADDON_MCP2515,
                                         .charger = CAN_NATIVE,
                                         .shunt = CAN_NATIVE};

struct CanReceiverRegistration {
  CanReceiver* receiver;
  CAN_Speed speed;
};

static std::multimap<CAN_Interface, CanReceiverRegistration> can_receivers;

static void receive_frame_can_native();
static void receive_frame_can_addon();
static void receive_frame_canfd_addon();
static void receive_frame_canfd_addon_2();
static void poll_can_addon_speed_change();
static void map_can_frame_to_variable(CAN_frame* rx_frame, CAN_Interface interface);
static void print_can_frame(CAN_frame frame, CAN_Interface interface, frameDirection msgDir);
static uint32_t init_native_can(CAN_Speed speed, gpio_num_t tx_pin, gpio_num_t rx_pin);
static bool begin_canfd();
static bool begin_canfd_2();

void register_can_receiver(CanReceiver* receiver, CAN_Interface interface, CAN_Speed speed) {
  can_receivers.insert({interface, {receiver, speed}});
  DEBUG_PRINTF("CAN receiver registered, total: %d\n", can_receivers.size());
}

static ACAN_ESP32_Settings* settingsespcan = nullptr;
static CAN_Speed native_can_speed;

static uint32_t quartz_frequency;

static MCP2515_Lite* can2515 = nullptr;
/* Whether the 2515 is fit to use, the counterpart of native_can_initialized.
 *
 * A null can2515 already means "this interface is not there" - that is how a
 * FAILED boot init reads. This is the other state the native path has and the
 * 2515 did not: the chip is present and the object is alive, but its bitrate is
 * unknown after a speed change that did not take, so it must not be polled or
 * transmitted to until a later change succeeds.
 */
static bool can2515_initialized = false;
static SPIClass* SPI2515;

static SPIClass* SPI2517;
static ACAN2517FD* canfd = nullptr;
static ACAN2517FDSettings* settings2517;
static SPIClass* SPI2517_2;
static ACAN2517FD* canfd_2 = nullptr;
static ACAN2517FDSettings* settings2517_2;

static bool native_can_initialized = false;

/* CAN-FD is drained from the MCP2518FD's nINT interrupt, and the handler is
 * resident in IRAM.
 *
 * The library also offers a no-interrupt mode (INT pin 255), and this lineage
 * tried it: a 1 ms task read the pin level and ran the library's poll core.
 * That mode cannot be used on ESP32, for a reason inside the library rather
 * than in the task. With no interrupt pin, receive() polls too - it runs that
 * same poll core from inside its own SPI transaction, and on Arduino-ESP32 a
 * transaction is a plain FreeRTOS mutex taken with portMAX_DELAY. The nested
 * beginTransaction() re-takes the mutex the same task already holds, with
 * interrupts masked (the library's turnOffInterrupts() is
 * taskDISABLE_INTERRUPTS() on this core), so core_loop never comes back from
 * receive() and the task watchdog reboots the board five seconds later. The
 * path is reached the first time available() is true: an idle board lives, a
 * board with a battery on the bus reboot-loops at ordinary load. The same mode
 * also moves transmit frames only when the tick runs, which caps FD transmit
 * at one driver-buffer drain per tick.
 *
 * What the interrupt has to satisfy instead is the flash-cache rule: an
 * interrupt taken while a flash operation has the cache off must not fetch
 * from flash. The handler does exactly one thing - it gives the library's
 * semaphore, so the library's own handler task (a task, never interrupt
 * context) runs the drain - and everything it touches is resident: this
 * trampoline (IRAM_ATTR), the library's isr() (IRAM_ATTR on the ESP32 build),
 * xSemaphoreGiveFromISR (the framework is built with
 * CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH unset), and Arduino's GPIO
 * dispatcher (CONFIG_ARDUINO_ISR_IRAM=y in the shipping sdkconfig makes
 * __onPinInterrupt IRAM and installs the GPIO ISR service with
 * ESP_INTR_FLAG_IRAM). The canfd/canfd_2 pointers are DRAM. That chain is
 * audited per linked image by the notes repo's ISR IRAM audit, not by reading
 * source - -Os has emitted "inline" helpers out-of-line into flash before.
 */
static void IRAM_ATTR canfd_isr() {
  canfd->isr();
}

static void IRAM_ATTR canfd_2_isr() {
  canfd_2->isr();
}

/* Is the MCP2515 the only device on its SPI bus?
 *
 * The interrupt drain takes the bus at moments no task can be asked about, so
 * it is only offered when nothing else is on it. That is a stricter question
 * than contention: measuring it on this hardware showed that sharing does not
 * degrade, it breaks - a second
 * SPIClass::begin() on one controller re-routes MISO through the GPIO matrix,
 * which can source a peripheral input from exactly one pad, so the device that
 * called begin() first simply goes deaf. A shared bus is a broken bus, with or
 * without this drain.
 *
 * The SD check is deliberately compile-time-wide: SDCARD being built in is
 * enough to decline, because the two settings that start the SD writer are
 * runtime ones and may be turned on long after this decision is made.
 */
static bool mcp2515_bus_is_exclusive() {
  const uint8_t bus = esp32hal->MCP2515_BUS();

#ifdef SDCARD
  if (esp32hal->SD_SPI_BUS() == bus) {
    return false;
  }
#endif

  const bool fd_present = can_receivers.find(CANFD_ADDON_MCP2518) != can_receivers.end() ||
                          can_receivers.find(CANFD_NATIVE) != can_receivers.end();
  if (fd_present && esp32hal->MCP2517_BUS() == bus) {
    return false;
  }
  if (can_receivers.find(CANFD_ADDON_MCP2518_2) != can_receivers.end() && esp32hal->MCP2517_BUS2() == bus) {
    return false;
  }

  return true;
}

// CAN_Interface (the runtime channel) back to comm_interface (what a board DECLARES it has).
// comm_nvm.cpp maps the other direction when it reads the settings; this is the inverse, kept
// local because the only caller is the availability check below.
static comm_interface comm_interface_for(CAN_Interface interface) {
  switch (interface) {
    case CAN_Interface::CAN_NATIVE:
      return comm_interface::CanNative;
    case CAN_Interface::CANFD_NATIVE:
      return comm_interface::CanFdNative;
    case CAN_Interface::CAN_ADDON_MCP2515:
      return comm_interface::CanAddonMcp2515;
    case CAN_Interface::CANFD_ADDON_MCP2518:
      return comm_interface::CanFdAddonMcp2518;
    case CAN_Interface::CANFD_ADDON_MCP2518_2:
      return comm_interface::CanFdAddonMcp2518_2;
    default:
      return comm_interface::Highest;  // no declaration can match, so it is refused
  }
}

/* An interface that failed to start is also removed from the receiver map.
 *
 * Leaving the pointer null (or the native flag false) is what keeps the rest of
 * this file from talking to a chip that is not there, and that alone was the
 * previous behaviour. It is not quite enough: a driver that registered on the
 * interface stays registered on it, so nothing downstream can tell "no traffic
 * yet" from "this channel was never brought up". Erasing the entry and raising
 * EVENT_INTERFACE_MISSING says which of the two it is, on the channel the rest
 * of the firmware already reads.
 *
 * It is deliberately NOT an abort. The caller discards init_CAN()'s outcome
 * (Software.cpp), so ending the function here would only skip the interfaces
 * declared after this one, silently.
 */
static void interface_unavailable(CAN_Interface interface) {
  set_event(EVENT_INTERFACE_MISSING, (uint8_t)interface);
  logging.printf("CAN interface %s did not initialize - continuing without it\n", getCANInterfaceName(interface));
  can_receivers.erase(interface);
}

#ifdef UNIT_TEST
// Puts the CAN layer back to "nothing registered, no chip initialized". The
// state below is file-static and the host suite runs every case in one process,
// so without this a test that brings an interface up decides what the next one
// sees.
void comm_can_reset_for_test() {
  can_receivers.clear();
  settingsespcan = nullptr;
  native_can_initialized = false;
  // The 2515's counterpart to native_can_initialized, added on this lane after
  // this hook was written: the transmit path reads it, so a suite that leaves
  // it standing lets one case's successful bring-up decide the next case's
  // "is this interface usable".
  can2515_initialized = false;
  can2515 = nullptr;
  SPI2515 = nullptr;
  canfd = nullptr;
  settings2517 = nullptr;
  canfd_2 = nullptr;
  settings2517_2 = nullptr;
  SPI2517 = nullptr;
  SPI2517_2 = nullptr;
  user_selected_CAN_ID_cutoff_filter = 0;
}
#endif  // UNIT_TEST

/* One chip's failure stops at that chip.
 *
 * This function used to `return false` on any failure, which read as "fail
 * loudly" but could not: the return is discarded at the only call site
 * (Software.cpp), so the abort neither stopped the boot nor told anyone - it
 * silently skipped every interface declared after the one that failed. It did
 * not prevent a half-initialised set either; it only changed WHICH interfaces
 * survived, and that was decided by the order they happen to be initialised in.
 * Declaration order is not a safety property.
 *
 * Chip init failures are now per-interface: raise that interface's event, leave
 * its pointer null, carry on. Null is what the rest of this file already treats
 * as "not there" - receive_can() and the transmit paths all guard on it - so a
 * failed chip is inert rather than absent-and-unmentioned.
 *
 * PIN ALLOCATION failures are per-interface too, and used to be the one
 * exception. The argument for the exception was that carrying on would hand the
 * same pad to whichever interface asks next - and alloc_pins() itself refutes
 * it: it validates every requested pin in a first loop and records them in a
 * second, so a call that fails records NOTHING. The pad in conflict stays with
 * whoever claimed it first, and the next interface asking for it gets the same
 * refusal. Nothing is ever double-handed, with or without the abort.
 *
 * The exception also lumped two different facts together. EVENT_GPIO_CONFLICT
 * (two functions claiming one pad) is plausibly a board-declaration bug;
 * EVENT_GPIO_NOT_DEFINED fires when a requested pin is GPIO_NUM_NC, which is
 * not a broken board at all - it is a user selecting an interface this board
 * does not route, from a dropdown that offered it. Both are reachable from the
 * settings UI on shipping boards: a DFRobot Edge101 offers both MCP add-ons
 * while declaring no MCP pins, and a 3LB declares MCP2515_MISO and MCP2517_INT
 * as the same pad. Aborting cost every interface ordered after the failing one,
 * which is the same defect this function's first paragraph describes, produced
 * by its own pin policy.
 *
 * So a failed allocation now leaves that interface out of service - null
 * pointer, or the native flag false - and initialisation carries on. The one
 * failure that is not confined to a single interface is the shared MCP2517 SPI
 * bus, because nothing can talk over a bus that was never opened; it stops at
 * the FD chips that would use that bus. alloc_pins() stays loud on its own:
 * it raises EVENT_GPIO_CONFLICT or EVENT_GPIO_NOT_DEFINED before returning.
 *
 * The return type is gone rather than made meaningful. Every failure now has an
 * event, which is the channel the rest of the firmware already reads; a bool
 * nobody examines was the thing that made "it fails loudly" look true.
 *
 * Ahead of all of it, an interface the BOARD does not declare is refused rather
 * than initialised - see the comment on that loop. That refusal, like every
 * failure below it, costs only the interface it names.
 */
void init_CAN() {
  /* Refuse an interface this board does not have, rather than initialising it and failing
   * obscurely.
   *
   * Selecting an interface whose chip select IS routed but carries no chip used to report
   * "autodetected crystal: 0MHz" followed by "CAN-FD 2 Configuration error 0x1" - a message
   * about a crystal, for a chip that is not fitted. Selecting one whose chip select is NOT
   * routed fails differently: alloc_pins() refuses the negative pin, and the interface is
   * left out of service for a reason that reads as a pin fault rather than as a board that
   * has no such chip. The board already declares what it has; consult it first and say so
   * plainly.
   */
  const auto available = esp32hal->available_interfaces();
  for (auto it = can_receivers.begin(); it != can_receivers.end();) {
    if (std::find(available.begin(), available.end(), comm_interface_for(it->first)) == available.end()) {
      // Drop THIS interface and carry on. Returning here would abandon the
      // whole of init_CAN() before anything was initialised, so one stale
      // selection would leave the board with NO CAN at all - including a
      // perfectly good native channel - and Software.cpp discards the return
      // value, so the only trace would be an event. Refusing the one thing that
      // is missing is the behaviour the check is for.
      logging.printf("CAN interface %s is not available on this board - refusing to initialize it\n",
                     getCANInterfaceName(it->first));
      set_event(EVENT_INTERFACE_MISSING, (uint8_t)it->first);
      it = can_receivers.erase(it);
    } else {
      ++it;
    }
  }

  // Native CAN (onboard the ESP32)

  auto nativeIt = can_receivers.find(CAN_NATIVE);

  const bool native_ok = nativeIt == can_receivers.end() || [&]() -> bool {
    auto se_pin = esp32hal->CAN_SE_PIN();
    auto tx_pin = esp32hal->CAN_TX_PIN();
    auto rx_pin = esp32hal->CAN_RX_PIN();

    bool pins_ok = true;

    if (se_pin != GPIO_NUM_NC) {
      if (!esp32hal->alloc_pins("CAN", se_pin)) {
        pins_ok = false;
      } else {
        pinMode(se_pin, OUTPUT);
        digitalWrite(se_pin, LOW);
      }
    }

    if (pins_ok && !esp32hal->alloc_pins("CAN", tx_pin, rx_pin)) {
      pins_ok = false;
    }

    if (!pins_ok) {
      // alloc_pins() has already raised EVENT_GPIO_NOT_DEFINED or
      // EVENT_GPIO_CONFLICT. Leaving the flag false is what takes this
      // interface out of service - receive_can() and both transmit paths read
      // it - and every interface declared after this one still gets its turn.
      native_can_initialized = false;
    } else {
      const uint32_t errorCode = init_native_can(nativeIt->second.speed, tx_pin, rx_pin);
      if (errorCode == 0) {
        native_can_initialized = true;
        logging.println("Native Can ok");
        logging.print("Bit Rate prescaler: ");
        logging.println(settingsespcan->mBitRatePrescaler);
        logging.print("Time Segment 1:     ");
        logging.println(settingsespcan->mTimeSegment1);
        logging.print("Time Segment 2:     ");
        logging.println(settingsespcan->mTimeSegment2);
        logging.print("RJW:                ");
        logging.println(settingsespcan->mRJW);
        logging.print("Triple Sampling:    ");
        logging.println(settingsespcan->mTripleSampling ? "yes" : "no");
        logging.print("Actual bit rate:    ");
        logging.print(settingsespcan->actualBitRate());
        logging.println(" bit/s");
        logging.print("Exact bit rate ?    ");
        logging.println(settingsespcan->exactBitRate() ? "yes" : "no");
        logging.print("Sample point:       ");
        logging.print(settingsespcan->samplePointFromBitStart());
        logging.println("%");
      } else {
        logging.print("Error Native Can: 0x");
        logging.println(errorCode, HEX);
        // This path had no event, only a log - and these boards log
        // nothing unless USBENABLED is set, so the failure that aborted every
        // other interface was also the only one nobody could see.
        set_event(EVENT_CAN_NATIVE_INIT_FAILURE, (uint8_t)errorCode);
        native_can_initialized = false;
      }
    }
    return native_can_initialized;
  }();
  if (nativeIt != can_receivers.end() && !native_ok) {
    interface_unavailable(CAN_NATIVE);
  }

  // Add-on CAN interface (via MCP2515)

  auto addonIt = can_receivers.find(CAN_ADDON_MCP2515);

  /* An MCP2515 whose pads this board does not route is skipped and left inert,
   * rather than attempted and reported as a pin fault. That is a different
   * question from the availability refusal at the top of this function: there
   * the BOARD does not declare the interface at all, here it declares it and
   * some of the pins it needs are GPIO_NUM_NC. Both leave the rest of the
   * bring-up alone.
   *
   * Absent is not the same as failed, so an absent add-on raises nothing and is
   * not marked unavailable - the gate below is what distinguishes the two.
   */
  const bool addon_present =
      addonIt != can_receivers.end() &&
      esp32hal->pins_present("CAN", esp32hal->MCP2515_CS(), esp32hal->MCP2515_INT(), esp32hal->MCP2515_SCK(),
                             esp32hal->MCP2515_MISO(), esp32hal->MCP2515_MOSI());

  const bool addon_ok = !addon_present || [&]() -> bool {
    auto cs_pin = esp32hal->MCP2515_CS();
    auto int_pin = esp32hal->MCP2515_INT();
    auto sck_pin = esp32hal->MCP2515_SCK();
    auto miso_pin = esp32hal->MCP2515_MISO();
    auto mosi_pin = esp32hal->MCP2515_MOSI();
    auto rst_pin = esp32hal->MCP2515_RST();

    if (!esp32hal->alloc_pins("CAN", cs_pin, int_pin, sck_pin, miso_pin, mosi_pin)) {
      // alloc_pins() has already raised the GPIO event, and it records nothing
      // when it fails, so no later interface can be handed a pad this one
      // asked for. Null is how the send and receive paths read "not there".
      can2515 = nullptr;
    } else {

      logging.println("Dual CAN Bus (ESP32+MCP2515) selected");

      if (rst_pin != GPIO_NUM_NC) {
        pinMode(rst_pin, OUTPUT);
        digitalWrite(rst_pin, HIGH);
        delay(100);
        digitalWrite(rst_pin, LOW);
        delay(100);
        digitalWrite(rst_pin, HIGH);
        delay(100);
      }

      SPI2515 = new SPIClass(esp32hal->MCP2515_BUS());
      SPI2515->begin(sck_pin, miso_pin, mosi_pin);
      can2515 = new MCP2515_Lite(*SPI2515, cs_pin, int_pin);

      if (mcp2515_bus_is_exclusive()) {
        can2515->useIsrDrain(esp32hal->MCP2515_BUS());
      }

      quartz_frequency = esp32hal->MCP2515_FREQ();
      if (quartz_frequency == 0) {
        quartz_frequency = can2515->autodetectOscillatorFrequency();
      }

      if (can2515->begin({(int)addonIt->second.speed * 1000UL, quartz_frequency})) {
        can2515_initialized = true;
        logging.println("MCP2515 CAN ok");
      } else {
        logging.println("MCP2515 CAN init failed");
        set_event(EVENT_CANMCP2515_INIT_FAILURE, 1);
        can2515_initialized = false;
        // This will leak, but we have failed and won't try to reinit. Null is how
        // the send and receive paths already read "this interface is not there".
        can2515 = nullptr;
      }
    }
    return can2515 != nullptr;
  }();
  if (addon_present && !addon_ok) {
    interface_unavailable(CAN_ADDON_MCP2515);
  }

  // FD interface(s) (via MCP2518FD)

  auto fdNativeIt = can_receivers.find(CANFD_NATIVE);
  auto fdAddonIt = can_receivers.find(CANFD_ADDON_MCP2518);
  auto fdAddonIt_2 = can_receivers.find(CANFD_ADDON_MCP2518_2);

  /* Which of the three FD blocks may run at all. An FD add-on the board does
   * not route is skipped and left inert, the same rule the MCP2515 block above
   * follows - but it cannot be decided block by block, because the bus block
   * creates the SPI object the two chip blocks dereference. plan_canfd_init()
   * carries that dependency, and lives in its own header so the host suite can
   * run it; comm_can.cpp itself does not link there.
   */
  const CanFdInitPlan fd_plan =
      plan_canfd_init(esp32hal, fdNativeIt != can_receivers.end() || fdAddonIt != can_receivers.end(),
                      fdAddonIt_2 != can_receivers.end());

  // A failure bringing up the shared FD bus is the one pin failure that is not
  // confined to a single interface: nothing can talk over a bus that was never
  // opened. It still stops there - the native and MCP2515 interfaces above are
  // already up, and the second FD chip is only affected when it shares this bus.
  bool fd_bus_ok = true;

  if (fd_plan.bus) {
    // Initialise SPI bus first
    auto sck_pin = esp32hal->MCP2517_SCK();
    auto sdo_pin = esp32hal->MCP2517_SDO();
    auto sdi_pin = esp32hal->MCP2517_SDI();

    if (!esp32hal->alloc_pins("CANFD", sck_pin, sdo_pin, sdi_pin)) {
      // This one failure does reach both FD chips, because nothing can use a
      // bus that was never brought up - but it reaches only the interfaces
      // that would share this bus, not the native or MCP2515 ones above.
      fd_bus_ok = false;
    } else {
      SPI2517 = new SPIClass(esp32hal->MCP2517_BUS());
      SPI2517->begin(sck_pin, sdo_pin, sdi_pin);
    }
  }

  if (!fd_bus_ok) {
    /* Only the interfaces that would have USED this bus. The second FD chip
     * brings up its own whenever MCP2517_BUS2() names a different controller,
     * and the block below is written for exactly that case - its pin gate reads
     * `fd_bus_ok || !shares_first_fd_bus`. Removing its registration here made
     * that clause unreachable: the re-read a few lines down turns the iterator
     * into end(), the gate short-circuits true, and the chip is reported
     * missing without ever having been attempted. plan_canfd_init() draws the
     * same line one level up (CanFdInitPlanTest.
     * ASecondChipOnItsOwnBusIsNotToldTheFirstBusIsMissing), so this loop was the
     * one place on the lane that did not.
     */
    const bool second_shares_this_bus = esp32hal->MCP2517_BUS() == esp32hal->MCP2517_BUS2();
    for (CAN_Interface fd : {CANFD_NATIVE, CANFD_ADDON_MCP2518, CANFD_ADDON_MCP2518_2}) {
      if (fd == CANFD_ADDON_MCP2518_2 && !second_shares_this_bus) {
        continue;
      }
      if (can_receivers.find(fd) != can_receivers.end()) {
        interface_unavailable(fd);
      }
    }
    // Re-read: the erases above invalidate exactly the iterators for what was
    // removed, and the blocks below test these against end().
    fdNativeIt = can_receivers.find(CANFD_NATIVE);
    fdAddonIt = can_receivers.find(CANFD_ADDON_MCP2518);
    fdAddonIt_2 = can_receivers.find(CANFD_ADDON_MCP2518_2);
  }

  const bool fd_ok =
      (fdNativeIt == can_receivers.end() && fdAddonIt == can_receivers.end()) || !fd_plan.first_chip || [&]() -> bool {
    auto speed = (fdNativeIt != can_receivers.end()) ? fdNativeIt->second.speed : fdAddonIt->second.speed;

    auto cs_pin = esp32hal->MCP2517_CS();
    auto int_pin = esp32hal->MCP2517_INT();

    // The short circuit matters: claiming cs/int for an interface that cannot
    // start would deny those pads to whoever asks next.
    const bool pins_ok = fd_bus_ok && esp32hal->alloc_pins("CANFD", cs_pin, int_pin);

    if (!pins_ok) {
      // The shared bus never came up, or this chip's own pads are unavailable
      // and alloc_pins() has raised the GPIO event. Null either way.
      canfd = nullptr;
    } else {
      canfd = new ACAN2517FD(cs_pin, *SPI2517, int_pin);

      logging.println("CAN FD add-on (ESP32+MCP2517) selected");

      const uint32_t freq = esp32hal->MCP2517_FREQ();
      ACAN2517FDSettings::Oscillator osc_freq =
          (freq == 0 ? ACAN2517FDSettings::OSC_AUTODETECT
                     : (freq == 20000000 ? ACAN2517FDSettings::OSC_20MHz : ACAN2517FDSettings::OSC_40MHz));
      auto bitRate = (int)speed * 1000UL;
      settings2517 = new ACAN2517FDSettings(osc_freq, bitRate, DataBitRateFactor::x4);

      // Set up clock output divider (some hardware uses this for the second CAN FD add-on)
      settings2517->mCLKOPin = static_cast<ACAN2517FDSettings::CLKOpin>(esp32hal->MCP2517_CLKODIV());

      // ListenOnly / Normal20B / NormalFDs
      settings2517->mRequestedMode =
          ACAN2517FDSettings::NormalFD;  //Startup in NormalFD mode, both for Classic CAN and CAN-FD messages

      // Deep enough to carry the bus through a flash write; see can_rx_ring_depth.h.
      settings2517->mDriverReceiveFIFOSize = CAN_DRIVER_RX_RING_DEPTH;

      if (!begin_canfd()) {
        // begin_canfd() has already raised EVENT_CANMCP2518FD_INIT_FAILURE.
        canfd = nullptr;
      }
    }
    return canfd != nullptr;
  }();
  if (!fd_ok) {
    if (fdNativeIt != can_receivers.end()) {
      interface_unavailable(CANFD_NATIVE);
    }
    if (fdAddonIt != can_receivers.end()) {
      interface_unavailable(CANFD_ADDON_MCP2518);
    }
  }

  const bool fd2_ok = fdAddonIt_2 == can_receivers.end() || !fd_plan.second_chip || [&]() -> bool {
    auto cs_pin = esp32hal->MCP2517_CS2();
    auto int_pin = esp32hal->MCP2517_INT2();

    // This chip shares the first FD bus only when the two bus numbers match;
    // when they differ it brings up its own, so a shared bus that never came
    // up decides nothing for it.
    const bool shares_first_fd_bus = esp32hal->MCP2517_BUS() == esp32hal->MCP2517_BUS2();

    bool pins_ok = (fd_bus_ok || !shares_first_fd_bus) && esp32hal->alloc_pins("CANFD2", cs_pin, int_pin);

    if (pins_ok) {
      if (shares_first_fd_bus) {
        // Use the same bus for both CAN FD chips
        SPI2517_2 = SPI2517;
      } else {
        auto sck_pin = esp32hal->MCP2517_SCK2();
        auto sdo_pin = esp32hal->MCP2517_SDO2();
        auto sdi_pin = esp32hal->MCP2517_SDI2();

        // Claimed before the bus object is built: the old order allocated a
        // SPIClass and then walked away from it on the failure path.
        if (!esp32hal->alloc_pins("CANFD2", sck_pin, sdo_pin, sdi_pin)) {
          pins_ok = false;
        } else {
          SPI2517_2 = new SPIClass(esp32hal->MCP2517_BUS2());
          SPI2517_2->begin(sck_pin, sdo_pin, sdi_pin);
        }
      }
    }

    if (!pins_ok) {
      // alloc_pins() has already raised the GPIO event; null is how the send
      // and receive paths read "this interface is not there".
      canfd_2 = nullptr;
    } else {
      canfd_2 = new ACAN2517FD(cs_pin, *SPI2517_2, int_pin);

      logging.println("CAN FD add-on 2 (ESP32+MCP2517) selected");

      const uint32_t freq = esp32hal->MCP2517_FREQ2();
      ACAN2517FDSettings::Oscillator osc_freq =
          (freq == 0 ? ACAN2517FDSettings::OSC_AUTODETECT
                     : (freq == 20000000 ? ACAN2517FDSettings::OSC_20MHz : ACAN2517FDSettings::OSC_40MHz));

      auto speed = fdAddonIt_2->second.speed;
      auto bitRate = (int)speed * 1000UL;
      // Crystal setting is ignored (library now autodetects)
      settings2517_2 = new ACAN2517FDSettings(osc_freq, bitRate, DataBitRateFactor::x4);
      // Arbitration bit rate: 250/500 kbit/s, data bit rate: 1/2 Mbit/s

      settings2517_2->mRequestedMode =
          ACAN2517FDSettings::NormalFD;  //Startup in NormalFD mode, both for Classic CAN and CAN-FD messages

      settings2517_2->mDriverReceiveFIFOSize = CAN_DRIVER_RX_RING_DEPTH;

      if (!begin_canfd_2()) {
        // begin_canfd_2() has already raised EVENT_CANMCP2518FD_INIT_FAILURE.
        canfd_2 = nullptr;
      }
    }
    return canfd_2 != nullptr;
  }();
  if (fdAddonIt_2 != can_receivers.end() && !fd2_ok) {
    interface_unavailable(CANFD_ADDON_MCP2518_2);
  }
}

static bool begin_canfd() {
  const uint32_t errorCode2517 = canfd->begin(*settings2517, canfd_isr);
  canfd->poll();
  if (errorCode2517 != 0) {
    logging.print("CAN-FD Configuration error 0x");
    logging.println(errorCode2517, HEX);
    set_event(EVENT_CANMCP2518FD_INIT_FAILURE, canfd_init_error_index(errorCode2517));
    // begin() attaches the nINT handler and starts the driver's task before it reports a
    // requested-mode timeout, so a non-zero code does not mean the driver is inert. end()
    // detaches the handler, stops that task and resets the chip; without it the next falling
    // edge on nINT runs the callback above against the pointer we are about to clear.
    canfd->end();
    // The driver object itself still leaks, but we have failed and won't try to reinit.
    canfd = nullptr;
    return false;
  }
  return true;
}

static bool begin_canfd_2() {
  const uint32_t errorCode2517_2 = canfd_2->begin(*settings2517_2, canfd_2_isr);
  canfd_2->poll();
  if (errorCode2517_2 != 0) {
    logging.print("CAN-FD 2 Configuration error 0x");
    logging.println(errorCode2517_2, HEX);
    set_event(EVENT_CANMCP2518FD_INIT_FAILURE, canfd_init_error_index(errorCode2517_2));
    // See begin_canfd(): a non-zero code can still leave the nINT handler attached.
    canfd_2->end();
    // The driver object itself still leaks, but we have failed and won't try to reinit.
    canfd_2 = nullptr;
    return false;
  }
  return true;
}

void transmit_can_frame_to_interface(const CAN_frame* tx_frame, CAN_Interface interface) {
  if (!allowed_to_send_CAN) {
    return;
  }
  print_can_frame(*tx_frame, interface, frameDirection(MSG_TX));

#ifdef SDCARD
  if (datalayer.system.info.CAN_SD_logging_active) {
    add_can_frame_to_buffer(*tx_frame, interface, frameDirection(MSG_TX));
  }
#endif

  switch (interface) {
    case CAN_NATIVE: {
      if (!native_can_initialized) {
        /* The TWAI peripheral was never taken out of reset, so its registers must not be
         * touched. tryToSend() writes them from inside portENTER_CRITICAL, which turns the
         * resulting exception into a DOUBLE exception - taken with interrupts off, so it
         * cannot be handled - and the watchdog reboots straight back into the same transmit.
         * Measured: roughly 45 resets a minute, indefinitely, and on S3 boards every one of
         * those boots is another chance to lose the USB port.
         *
         * Every other interface below already refuses this way; they hold a driver pointer
         * and short-circuit on null, while this one has a flag, and the flag was only ever
         * read by receive_can(). Dropping the frame and reporting it is what the null checks
         * do, so a dead native interface is now inert in both directions.
         */
        datalayer.system.info.can_native_not_initialized = true;
        break;
      }
      if (tx_frame->DLC > sizeof(CANMessage::data)) {
        // An FD-length frame cannot be sent on a classic CAN interface (a CAN-FD
        // battery configured on it produces these), and copying it below would
        // overflow frame.data on the stack.
        datalayer.system.info.can_native_send_fail = true;
        break;
      }
      CANMessage frame;
      frame.id = tx_frame->ID;
      frame.ext = tx_frame->ext_ID;
      frame.len = tx_frame->DLC;
      for (uint8_t i = 0; i < frame.len; i++) {
        frame.data[i] = tx_frame->data.u8[i];
      }

      if (!ACAN_ESP32::can.tryToSend(frame)) {
        datalayer.system.info.can_native_send_fail = true;
      }
    } break;
    case CAN_ADDON_MCP2515: {
      if (can2515 == nullptr) {
        // The chip never came up - init_CAN() nulled the pointer after begin() failed, or the
        // add-on was never configured. Dropping the frame here is what the null short-circuit
        // below already did; what changes is what the user is told. Folding it into the send
        // check reported EVENT_CANMCP2515_BUFFER_FULL, whose message is "Buffer full or no one
        // on the bus to ACK the message!", and sent someone looking at bus wiring for a chip
        // that is not there. The boot-time init failure already said what happened; this says
        // the same thing in the language of the frame that just went nowhere.
        datalayer.system.info.can_2515_not_initialized = true;
        break;
      }
      if (tx_frame->DLC > sizeof(MCP2515_Lite_Frame::data)) {
        // Same as CAN_NATIVE: an FD-length frame cannot travel over the MCP2515.
        datalayer.system.info.can_2515_send_fail = true;
        break;
      }
      MCP2515_Lite_Frame mcp2515_frame;
      copy_can_frame_to_mcp2515_lite_frame(*tx_frame, mcp2515_frame);

      // Not merely "is the chip there" but "is it usable": after a speed change
      // that did not take, the bitrate is unknown and transmitting onto a bus at
      // the wrong speed is worse than not transmitting at all.
      if (!can2515_initialized || !can2515->sendFrame(mcp2515_frame)) {
        datalayer.system.info.can_2515_send_fail = true;
      }
    } break;
    case CANFD_NATIVE:
    case CANFD_ADDON_MCP2518: {
      if (canfd == nullptr) {
        // See the MCP2515 case: same drop, honest diagnosis instead of "buffer full".
        datalayer.system.info.can_2518_not_initialized = true;
        break;
      }
      CANFDMessage MCP2518Frame;
      if (tx_frame->FD) {
        MCP2518Frame.type = CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
      } else {  //Classic CAN message
        MCP2518Frame.type = CANFDMessage::CAN_DATA;
      }
      MCP2518Frame.id = tx_frame->ID;
      MCP2518Frame.ext = tx_frame->ext_ID;
      MCP2518Frame.len = tx_frame->DLC;
      memcpy(MCP2518Frame.data, tx_frame->data.u8, std::min(tx_frame->DLC, (uint8_t)sizeof(MCP2518Frame.data)));

      if (!canfd->tryToSend(MCP2518Frame)) {
        datalayer.system.info.can_2518_send_fail = true;
      }
    } break;
    case CANFD_ADDON_MCP2518_2: {
      if (canfd_2 == nullptr) {
        // See the MCP2515 case: same drop, honest diagnosis instead of "buffer full".
        datalayer.system.info.can_2518_2_not_initialized = true;
        break;
      }
      CANFDMessage MCP2518Frame;
      if (tx_frame->FD) {
        MCP2518Frame.type = CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
      } else {  //Classic CAN message
        MCP2518Frame.type = CANFDMessage::CAN_DATA;
      }
      MCP2518Frame.id = tx_frame->ID;
      MCP2518Frame.ext = tx_frame->ext_ID;
      MCP2518Frame.len = tx_frame->DLC;
      memcpy(MCP2518Frame.data, tx_frame->data.u8, std::min(tx_frame->DLC, (uint8_t)sizeof(MCP2518Frame.data)));

      if (!canfd_2->tryToSend(MCP2518Frame)) {
        datalayer.system.info.can_2518_2_send_fail = true;
      }
    } break;
    default:
      // Invalid interface sent with function call - the frame reached no wire.
      // The replay path validates before sending, so reaching this branch
      // means a code path handed over an interface that does not exist.
      set_event(EVENT_CAN_INTERFACE_UNAVAILABLE, (uint8_t)interface);
      break;
  }
}

/* "Is this interface usable", which is a stricter question than "is the chip
 * there" - and for the MCP2515 the two have different answers.
 *
 * The one caller is the CAN-replay interface check, and what it exists to stop
 * is a replay that reaches no wire while every page shows it transmitting. A
 * 2515 whose runtime speed change did not take is exactly that: the object is
 * alive, so the pointer says yes, and the transmit path then drops every frame
 * on !can2515_initialized and counts them as send failures. That is the symptom
 * the check was written against, arriving through the other door.
 *
 * The native case has always asked the usable question - native_can_initialized
 * is cleared by a failed change as well as by a failed init - and the 2515's
 * counterpart flag arrived later, on another branch, so this switch kept
 * answering with the pointer alone. Recovery still works: a later change that
 * takes sets the flag again, and this answers yes again with it.
 */
bool can_interface_ready(int interface) {
  switch (interface) {
    case CAN_NATIVE:
      return native_can_initialized;
    case CAN_ADDON_MCP2515:
      return can2515 != nullptr && can2515_initialized;
    case CANFD_NATIVE:
    case CANFD_ADDON_MCP2518:
      return canfd != nullptr;
    case CANFD_ADDON_MCP2518_2:
      return canfd_2 != nullptr;
    default:
      return false;
  }
}

// Receive functions
void receive_can() {
  if (native_can_initialized) {
    receive_frame_can_native();  // Receive CAN messages from native CAN port
  }

  if (can2515) {
    /* The speed-change verdict is polled OUTSIDE the usability gate, and that
     * ordering is load-bearing.
     *
     * The verdict arrives asynchronously on this path, so if it were read inside
     * the gate the gate would be a one-way door: a failed change clears the
     * flag, the poll stops running, and the success that would restore the
     * interface is never seen. Polling first is also what makes recovery mean
     * something - a later change that takes puts the interface straight back.
     */
    poll_can_addon_speed_change();

    if (can2515_initialized) {
      receive_frame_can_addon();  // Receive CAN messages on add-on MCP2515 chip
    }
  }

  if (canfd) {
    receive_frame_canfd_addon();  // Receive CAN-FD messages.
  }

  if (canfd_2) {
    receive_frame_canfd_addon_2();  // Receive CAN-FD messages on 2nd CAN-FD add-on.
  }
}

static void
receive_frame_can_native() {  // This section checks if we have a complete CAN message incoming on native CAN port
  CANMessage frame;

  /* Drain a BATCH, the way every other interface in this file already does.
   *
   * This used to take one frame per call, and since receive_can() runs once per
   * iteration of the 1 kHz core loop that made the application - not the bus, and
   * not the ISR - the ceiling: measured on silicon at 999 f/s received against
   * 3956 f/s offered on a 500 kbit bus, 74.7 % lost on an idle board, while the
   * same board paced to 400 f/s lost nothing. A pack streaming faster than about
   * one frame per millisecond was silently three-quarters unheard.
   *
   * The bound is the DRIVER RING'S OWN DEPTH, asked of the driver rather than
   * copied from the add-on paths' unexplained 16, and the reasoning is the
   * defect in miniature: the ring is the most the ISR can have queued since the
   * last call, so a cap at its depth always empties whatever accumulated and the
   * backlog cannot carry over. Any cap BELOW the depth can leave frames behind
   * on every iteration - which is exactly how one-per-call failed, just less
   * severely. Above it there is nothing left to take.
   *
   * receive() reports the empty ring itself, so the separate available() check
   * it used to be guarded by is gone: it cost a second critical section per call
   * to answer a question the very next line asks again.
   */
  const uint16_t drain_limit = ACAN_ESP32::can.driverReceiveBufferSize();
  uint16_t drained = 0;
  while (drained++ < drain_limit && ACAN_ESP32::can.receive(frame)) {
    CAN_frame rx_frame;
    rx_frame.ID = frame.id;
    rx_frame.ext_ID = frame.ext;
    rx_frame.DLC = frame.len;
    rx_frame.FD = false;
    for (uint8_t i = 0; i < frame.len && i < 8; i++) {
      rx_frame.data.u8[i] = frame.data[i];
    }

    //message incoming, pass it on to the handler
    map_can_frame_to_variable(&rx_frame, CAN_NATIVE);
  }

  auto flags = ACAN_ESP32::can.statusRegister();
  if ((flags & TWAI_BUS_OFF_ST) != 0) {
    // Bus off, reset the CAN controller
    change_can_speed(CAN_Interface::CAN_NATIVE, native_can_speed);
    datalayer.system.info.can_native_bus_error = true;
  }
  if ((flags & TWAI_ERR_ST) != 0) {
    datalayer.system.info.can_native_bus_error = true;
  }
}

static void
receive_frame_can_addon() {  // This section checks if we have a complete CAN message incoming on add-on CAN port
  MCP2515_Lite_Frame rx_frame;
  CAN_frame full_frame;

  int count = 0;
  while (count++ < 16 && can2515->receiveFrame(rx_frame)) {
    copy_mcp2515_lite_frame_to_can_frame(rx_frame, full_frame);
    map_can_frame_to_variable(&full_frame, CAN_ADDON_MCP2515);
  }

  if (can2515->hasErrors()) {
    datalayer.system.info.can_2515_bus_error = true;
  }
}

/* The 2515's speed-change verdict, asked for here because there is nowhere else
 * to ask.
 *
 * change_can_speed() hands the request to the driver task and returns; the task
 * enacts it milliseconds later, long after that caller is gone. So the status
 * cannot go back the way the request came, and it is picked up on the receive
 * path instead - which runs every cycle whether or not frames arrive.
 *
 * A failure is reported as the chip's init failure, the same event the boot path
 * raises, and for the same reason the native path reuses its own: an interface at
 * an unknown bitrate is not usable, however it got there. The readback change could only
 * report it; the interface stayed in service because nothing gated its use.
 * This change gives it that gate, so the report now also takes it OUT of service, and
 * a later change that succeeds brings it back - the same way the native path
 * recovers, where a good init sets native_can_initialized true again.
 */
static void poll_can_addon_speed_change() {
  if (can2515->speedChangeFailed()) {
    can2515_initialized = false;
    set_event(EVENT_CANMCP2515_INIT_FAILURE, 0);
  } else if (can2515->speedChangeSucceeded()) {
    can2515_initialized = true;
  }
}

static void _receive_frame_canfd(ACAN2517FD* canfd, bool first) {
  CANFDMessage MCP2518frame;
  int count = 0;
  while (canfd->available() && count++ < 16) {
    canfd->receive(MCP2518frame);

    CAN_frame rx_frame;
    rx_frame.ID = MCP2518frame.id;
    rx_frame.ext_ID = MCP2518frame.ext;
    rx_frame.DLC = MCP2518frame.len;
    rx_frame.FD = (MCP2518frame.type == CANFDMessage::CANFD_NO_BIT_RATE_SWITCH ||
                   MCP2518frame.type == CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH);
    memcpy(rx_frame.data.u8, MCP2518frame.data, std::min(rx_frame.DLC, (uint8_t)sizeof(rx_frame.data.u8)));
    //message incoming, pass it on to the handler
    if (first) {
      map_can_frame_to_variable(&rx_frame, CANFD_ADDON_MCP2518);
      map_can_frame_to_variable(&rx_frame, CANFD_NATIVE);
    } else {
      map_can_frame_to_variable(&rx_frame, CANFD_ADDON_MCP2518_2);
    }
  }

  if (canfd->hasCanErrors()) {
    if (first) {
      datalayer.system.info.can_2518_bus_error = true;
    } else {
      datalayer.system.info.can_2518_2_bus_error = true;
    }
  }
}

static void receive_frame_canfd_addon() {
  _receive_frame_canfd(canfd, true);
}

static void receive_frame_canfd_addon_2() {
  _receive_frame_canfd(canfd_2, false);
}

// Support functions
static void print_can_frame(CAN_frame frame, CAN_Interface interface, frameDirection msgDir) {

  if (datalayer.system.info.CAN_usb_logging_active) {
    // Build the whole line first, then write it in one go - and only if the TX
    // buffer has room. This path runs in the core task: a blocked/slow USB host
    // must never stall it (EVENT_TASK_OVERRUN). Frames that don't fit are
    // counted and reported as a gap marker once the port drains.
    static char usb_line[288];  // header + up to 64 CAN-FD data bytes at 3 chars each
    static uint32_t usb_frames_dropped = 0;
    unsigned long currentTime = millis();
    size_t size = snprintf(usb_line, sizeof(usb_line), "(%lu.%02lu) %s%d %lX [%u] ", currentTime / 1000,
                           (currentTime % 1000) / 10, (msgDir == MSG_RX) ? "RX" : "TX",
                           (msgDir == MSG_RX) ? (int)(interface * 2) : (int)(interface * 2) + 1, frame.ID, frame.DLC);
    for (uint8_t i = 0; i < frame.DLC; i++) {
      size += snprintf(usb_line + size, sizeof(usb_line) - size, (i < frame.DLC - 1) ? "%02X " : "%02X\r\n",
                       frame.data.u8[i]);
    }
    if (frame.DLC == 0) {
      size += snprintf(usb_line + size, sizeof(usb_line) - size, "\r\n");
    }

    if ((size_t)Serial.availableForWrite() >= size) {
      if (usb_frames_dropped > 0) {
        char marker[48];
        int marker_len =
            snprintf(marker, sizeof(marker), "[%lu CAN frames not printed]\r\n", (unsigned long)usb_frames_dropped);
        if ((size_t)Serial.availableForWrite() >= size + (size_t)marker_len) {
          Serial.write((const uint8_t*)marker, marker_len);
          usb_frames_dropped = 0;
        }
      }
      Serial.write((const uint8_t*)usb_line, size);
    } else {
      usb_frames_dropped++;
    }
  }

  if (datalayer.system.info.can_streaming_active) {
    stream_can_frame(frame, interface, msgDir);
  }
}

static void map_can_frame_to_variable(CAN_frame* rx_frame, CAN_Interface interface) {
  if (interface !=
      CANFD_NATIVE) {  //Avoid printing twice due to receive_frame_canfd_addon sending to both FD interfaces
    //TODO: This check can be removed later when refactored to use inline functions for logging
    print_can_frame(*rx_frame, interface, frameDirection(MSG_RX));
  }

#ifdef SDCARD
  if (datalayer.system.info.CAN_SD_logging_active) {
    if (interface !=
        CANFD_NATIVE) {  //Avoid printing twice due to receive_frame_canfd_addon sending to both FD interfaces
      //TODO: This check can be removed later when refactored to use inline functions for logging
      add_can_frame_to_buffer(*rx_frame, interface, frameDirection(MSG_RX));
    }
  }
#endif

  // Send the frame to all the receivers registered for this interface.
  auto receivers = can_receivers.equal_range(interface);

  for (auto it = receivers.first; it != receivers.second; ++it) {
    auto& receiver = it->second;
    receiver.receiver->receive_can_frame(rx_frame);
  }
}

// For formatting CAN frames considerably faster than using snprintf
static const char* hex = "0123456789abcdef";

static char* put_hex(char* ptr, uint32_t value, uint8_t digits) {
  for (int i = digits - 1; i >= 0; i--) {
    *ptr++ = hex[(value >> (i * 4)) & 0x0f];
  }
  return ptr;
}

static char* put_time(char* ptr, unsigned long time) {
  // Wrap around after 100000 seconds (about 27.7 hours)
  if (time >= 100000000)
    time = time % 100000000;

  char buf[8];
  int i = 0;
  do {
    buf[i++] = (time % 10) + '0';
    time /= 10;
  } while (time > 0);
  while (i > 0) {
    *ptr++ = buf[--i];
    if (i == 3) {
      *ptr++ = '.';
    }
  }
  return ptr;
}

// CAN log formatter: "(12345.678) RX0 123 [8] 01 02 03 ... 0A\n".
size_t format_can_frame(char* buffer, size_t len, const CAN_frame& frame, CAN_Interface interface,
                        frameDirection msgDir) {
  // Worst-case line length: '(' + up-to-8-digit time + optional '.' + ')' + ' '
  // + "RX"/"TX" + channel digit + ' ' + 8-hex ID + ' ' + '[' + 2-digit DLC + ']'
  // + 3 bytes per data byte + '\n'.
  const size_t needed = 1 + 9 + 1 + 1 + 3 + 1 + 8 + 1 + 1 + 2 + 1 + (size_t)frame.DLC * 3 + 1;
  if (needed > len) {
    if (len > 0) {
      buffer[0] = '\0';
    }
    return 0;
  }

  char* ptr = buffer;
  const unsigned long currentTime = millis();
  *ptr++ = '(';
  ptr = put_time(ptr, currentTime);
  *ptr++ = ')';
  *ptr++ = ' ';
  if (msgDir == MSG_RX) {
    *ptr++ = frame.FD ? 'R' : 'r';
    *ptr++ = frame.FD ? 'X' : 'x';
    *ptr++ = '0' + ((int)interface * 2);
  } else {
    *ptr++ = frame.FD ? 'T' : 't';
    *ptr++ = frame.FD ? 'X' : 'x';
    *ptr++ = '1' + ((int)interface * 2);
  }
  *ptr++ = ' ';
  if (frame.ext_ID)
    ptr = put_hex(ptr, frame.ID, 8);
  else
    ptr = put_hex(ptr, frame.ID, 3);
  *ptr++ = ' ';
  *ptr++ = '[';
  if (frame.DLC > 9) {
    *ptr++ = '0' + (frame.DLC / 10);
    *ptr++ = '0' + (frame.DLC % 10);
  } else
    *ptr++ = '0' + (frame.DLC);
  *ptr++ = ']';
  for (int i = 0; i < frame.DLC; i++) {
    *ptr++ = ' ';
    ptr = put_hex(ptr, frame.data.u8[i], 2);
  }
  *ptr++ = '\n';
  *ptr = '\0';
  return (size_t)(ptr - buffer);
}

CanDrainCounters can_drain_counters() {
  if (can2515 == nullptr || !can2515->isrDrainActive()) {
    return {false, 0, 0, 0, 0};
  }
  return {true, can2515->isrFramesDrained(), can2515->isrFramesDropped(), can2515->isrBusDeferrals(),
          can2515->isrBusTimeouts()};
}

void reset_can_drain_counters() {
  if (can2515 != nullptr) {
    can2515->resetIsrCounters();
  }
}

void stop_can() {
  /* Registration is not initialization. A driver registers on CAN_NATIVE before init_CAN()
   * runs, so on a board where the native init failed this condition is TRUE while the TWAI
   * peripheral was never enabled - the same state the transmit guard above exists for, and
   * end() writes TWAI_CMD_REG and TWAI_INT_ENA_REG before it disables the module.
   *
   * The flag goes down WITH the peripheral. end() calls
   * periph_module_disable(PERIPH_TWAI_MODULE), so leaving the flag set left the transmit
   * guard reading "the interface is up" about a module that is clock-gated - and
   * allowed_to_send_CAN does not cover the gap. CAN replay runs in its own FreeRTOS task
   * (webserver.cpp, xTaskCreatePinnedToCore "CAN_Replay"), so it can be inside
   * transmit_can_frame_to_interface(), past that check, while core_loop's 1 s sub-task
   * runs stop_can() here. Resuming is worse: update_pause_state() sets allowed_to_send_CAN
   * true BEFORE calling restart_can(), so every replayed frame in that window used to reach
   * tryToSend() on a disabled peripheral, which faults with interrupts off: a double
   * exception the watchdog reboots straight back into, ~45 resets a minute.
   * Clearing it here does not make the hand-off atomic, but it shrinks the exposure to the
   * few instructions between the guard and tryToSend(), which is exactly the guarantee the
   * null-pointer checks below give for the other three interfaces.
   *
   * receive_can() reads the same flag, so it now skips the native interface while paused
   * instead of draining what is left in the driver's software buffer. That loses nothing:
   * ACAN_ESP32::begin() re-runs mDriverReceiveBuffer.initWithSize(), so those frames were
   * discarded on resume either way - and delivering frames captured before a pause INTO the
   * pause, updating the datalayer while the emulator is deliberately quiet, was the odder of
   * the two behaviours.
   */
  if (native_can_initialized) {
    ACAN_ESP32::can.end();
    native_can_initialized = false;
  }

  if (can2515) {
    can2515->pause(true);
  }

  if (canfd) {
    canfd->end();
  }

  if (canfd_2) {
    canfd_2->end();
  }
}

void restart_can() {
  /* Gate on the settings pointer, not on the flag. stop_can() has just cleared the
   * flag, and this is the function whose job is to undo that, so reading it here would make
   * resuming a no-op and leave native CAN down for good after the first pause.
   *
   * settingsespcan is the same guard change_can_speed() uses below, and it answers the
   * question this line actually asks - is there a native interface to bring back. It is
   * assigned only inside init_native_can(), and init_CAN()'s pin-failure branch skips that
   * call, so it stays null on exactly the pin-conflict boards where dereferencing it used to
   * crash the resume. That was true when a failed allocation returned out of init_CAN() and
   * it is still true now that it only takes the native interface out of service: what
   * matters here is that init_native_can() is not reached, not how it is avoided.
   *
   * The error code is no longer discarded. ACAN_ESP32::begin() reports its failure the same
   * way init_native_can() does, and until now this was the one (re)start that neither told
   * the flag nor raised the event - so a board whose boot-time native init failed could have
   * the peripheral brought up here and still be refused in both directions, with nothing
   * saying why. Taking the flag from the result also makes this a retry: an interface that
   * failed at boot and starts cleanly now goes back into service.
   */
  if (settingsespcan != nullptr) {
    const uint32_t errorCode = ACAN_ESP32::can.begin(*settingsespcan);
    native_can_initialized = (errorCode == 0);
    if (errorCode != 0) {
      logging.print("Error Native Can: 0x");
      logging.println(errorCode, HEX);
      set_event(EVENT_CAN_NATIVE_INIT_FAILURE, (uint8_t)errorCode);
    }
  }

  if (can2515) {
    can2515->pause(false);
  }

  if (canfd) {
    begin_canfd();
  }

  if (canfd_2) {
    begin_canfd_2();
  }
}

// Initialize the native CAN interface with the given speed and pins.
// This can be called repeatedly to change the interface speed (as some
// batteries require).
static uint32_t init_native_can(CAN_Speed speed, gpio_num_t tx_pin, gpio_num_t rx_pin) {

  // TODO: check whether this is necessary? It seems to help with
  // reinitialization.
  periph_module_reset(PERIPH_TWAI_MODULE);

  if (settingsespcan != nullptr) {
    delete settingsespcan;
  }

  native_can_speed = speed;

  // Create a new settings object (as it does the bitrate calcs in the constructor)
  settingsespcan = new ACAN_ESP32_Settings((int)speed * 1000UL);
  settingsespcan->mRequestedCANMode = ACAN_ESP32_Settings::NormalMode;
  settingsespcan->mTxPin = tx_pin;
  settingsespcan->mRxPin = rx_pin;
  // A new settings object on every speed change, so the depth is set here rather
  // than once: see can_rx_ring_depth.h.
  settingsespcan->mDriverReceiveBufferSize = CAN_DRIVER_RX_RING_DEPTH;

  // (Re)start the CAN interface
  return ACAN_ESP32::can.begin(*settingsespcan);
}

// Change the speed of the given CAN interface. Returns true if successful.
bool change_can_speed(CAN_Interface interface, CAN_Speed speed) {
  if (interface == CAN_Interface::CAN_NATIVE && settingsespcan != nullptr) {
    // Reinitialize the native CAN interface with the new speed
    const uint32_t errorCode = init_native_can(speed, settingsespcan->mTxPin, settingsespcan->mRxPin);
    if (errorCode != 0) {
      /* The interface is DOWN, so say so in the flag receive_can() reads.
       *
       * init_native_can() has already failed here, exactly as it can at boot -
       * and the boot path clears this flag and raises the event. This one used
       * to do neither, so after a failed runtime speed change the firmware kept
       * calling receive_frame_can_native() on an interface whose begin() had
       * just failed. Same class as the defect fixed in init_CAN() one function up: the
       * failure is reported through a return value, while the state that
       * decides whether the interface is USED still says it is fine.
       *
       * The flag is also what can_interface_ready() answers with, so leaving it
       * standing would let a CAN replay that was validated at start go back to
       * sending into an interface that has since died. This is driver-stack
       * code and no host test reaches the line - which is why the readiness
       * seam the replay validation uses is injectable.
       */
      native_can_initialized = false;
      logging.print("Error Native Can: 0x");
      logging.println(errorCode, HEX);
      set_event(EVENT_CAN_NATIVE_INIT_FAILURE, (uint8_t)errorCode);
      return false;
    }
    native_can_initialized = true;
    return true;
  } else if (interface == CAN_Interface::CAN_ADDON_MCP2515 && can2515) {
    /* true here means the request was accepted, not that the speed changed:
     * changeSpeed() hands it to the driver task and returns. That used to be
     * indistinguishable from a change that worked, because no status existed
     * anywhere in the chain. It does now - the task verifies the chip
     * and poll_can_addon_speed_change() turns a failed verdict into an event.
     */
    can2515->changeSpeed({(int)speed * 1000UL, quartz_frequency});
    return true;
  }

  return false;
}
