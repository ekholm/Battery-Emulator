// HW_STARK before any include: types.h gates the GPIOOPT5 enum (and hw_stark.h
// needs it) behind this define. hal.cpp is a separate TU and keeps the build's
// single-board selection; the "multiple HW" guards are #undef'd per include
// below.
#define HW_STARK

#include <gtest/gtest.h>

#include "../Software/src/devboard/utils/pin_exclusions.h"

#include "../Software/src/devboard/utils/types.h"
#include "Arduino.h"

// The board matrix: every HAL header is pulled into this one TU so each
// board's declared pin pair is checked against the exclusion table on the
// host. The headers each end with `#define HalClass <X>`, hence the #undef
// chain; the class definitions themselves have distinct names.
// Classic-ESP32 boards only: the host emul's gpio enum stops where the
// classic chip does, so the S3 boards (BECom, LilyGo 2CAN, Waveshare,
// Edge101) do not compile here. Their pin pairs were read at implementation
// time and wire the roles apart (or not at all); the predicate is
// board-generic either way.
#undef HalClass
#undef HW_CONFIGURED
#include "../Software/src/devboard/hal/hw_lilygo.h"
#undef HalClass
#undef HW_CONFIGURED
#include "../Software/src/devboard/hal/hw_3LB.h"
#undef HalClass
#undef HW_CONFIGURED
#include "../Software/src/devboard/hal/hw_devkit.h"
#undef HalClass
#undef HW_CONFIGURED
#include "../Software/src/devboard/hal/hw_stark.h"
#undef HalClass
#undef HW_CONFIGURED

// The production definition of this global is compiled out under the host's
// HW_LILYGO build, so the matrix TU provides it (Stark's default option).
GPIOOPT5 user_selected_gpioopt5 = GPIOOPT5::DEFAULT_BMS_POWER_23;

namespace {

// ── The predicate itself ───────────────────────────────────────────────────

TEST(PinExclusionTest, SmaWithEquipmentStopOnSharedPinIsRefused) {
  EXPECT_NE(pin_exclusion_conflict(InverterProtocolType::SmaBydH, STOP_BUTTON_BEHAVIOR::LATCHING_SWITCH, GPIO_NUM_2,
                                   GPIO_NUM_2),
            nullptr);
  EXPECT_NE(pin_exclusion_conflict(InverterProtocolType::SmaBydHvs, STOP_BUTTON_BEHAVIOR::MOMENTARY_SWITCH, GPIO_NUM_2,
                                   GPIO_NUM_2),
            nullptr);
  EXPECT_NE(pin_exclusion_conflict(InverterProtocolType::SmaSBSByd, STOP_BUTTON_BEHAVIOR::LATCHING_SWITCH, GPIO_NUM_2,
                                   GPIO_NUM_2),
            nullptr);
}

TEST(PinExclusionTest, EitherSideAloneIsLegal) {
  EXPECT_EQ(pin_exclusion_conflict(InverterProtocolType::SmaBydH, STOP_BUTTON_BEHAVIOR::NOT_CONNECTED, GPIO_NUM_2,
                                   GPIO_NUM_2),
            nullptr)
      << "an SMA inverter with the stop button not connected is the supported configuration";
  EXPECT_EQ(pin_exclusion_conflict(InverterProtocolType::BydModbus, STOP_BUTTON_BEHAVIOR::LATCHING_SWITCH, GPIO_NUM_2,
                                   GPIO_NUM_2),
            nullptr)
      << "the stop button with a non-SMA inverter is the supported configuration";
}

TEST(PinExclusionTest, DistinctPinsNeverConflict) {
  EXPECT_EQ(pin_exclusion_conflict(InverterProtocolType::SmaBydH, STOP_BUTTON_BEHAVIOR::LATCHING_SWITCH, GPIO_NUM_5,
                                   GPIO_NUM_35),
            nullptr)
      << "boards that wire the roles apart (LilyGo shape) must not be refused";
}

TEST(PinExclusionTest, SmaLvDoesNotClaimTheContactorPin) {
  // SMA LV is not an SmaInverterBase subclass and never allocates the pin.
  EXPECT_FALSE(inverter_uses_sma_contactor_pin(InverterProtocolType::SmaLv));
  EXPECT_EQ(pin_exclusion_conflict(InverterProtocolType::SmaLv, STOP_BUTTON_BEHAVIOR::LATCHING_SWITCH, GPIO_NUM_2,
                                   GPIO_NUM_2),
            nullptr);
}

TEST(PinExclusionTest, UnwiredPinsNeverConflict) {
  // GPIO_NUM_NC == GPIO_NUM_NC must not read as a collision (base-class
  // defaults, e.g. Edge101 wires neither role).
  EXPECT_EQ(pin_exclusion_conflict(InverterProtocolType::SmaBydH, STOP_BUTTON_BEHAVIOR::LATCHING_SWITCH, GPIO_NUM_NC,
                                   GPIO_NUM_NC),
            nullptr);
}

// ── The per-board table (row (c): the declared combinations are consistent) ─
// Expected verdicts are LITERAL, not derived from the pins at runtime - a
// header edit that moves either pin onto or off a collision fails here loudly
// and the change gets looked at rather than inherited.

template <typename Hal>
const char* board_conflict() {
  Hal hal;
  return pin_exclusion_conflict(InverterProtocolType::SmaBydH, STOP_BUTTON_BEHAVIOR::LATCHING_SWITCH,
                                hal.INVERTER_CONTACTOR_ENABLE_PIN(), hal.EQUIPMENT_STOP_PIN());
}

TEST(PinExclusionBoardMatrixTest, StarkSharesGpio2AndIsRefused) {
  // The ruled case: SMA contactor enable and equipment stop both on GPIO 2.
  EXPECT_NE(board_conflict<StarkHal>(), nullptr);
}

TEST(PinExclusionBoardMatrixTest, EveryOtherClassicBoardWiresTheRolesApart) {
  EXPECT_EQ(board_conflict<LilyGoHal>(), nullptr);  // GPIO 5 vs 35
  EXPECT_EQ(board_conflict<ThreeLBHal>(), nullptr);
  EXPECT_EQ(board_conflict<DevKitHal>(), nullptr);
}

}  // namespace
