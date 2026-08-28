#include <gtest/gtest.h>

#include "../Software/src/inverter/BYD-MODBUS.h"

// handle_static_data()'s write cursor was `static uint16_t i = 100`, so
// it survived the call and every instantiation after the first resumed where
// the previous one stopped (168, 236, ...). The identity/manufacturer strings
// then landed at the wrong addresses and scribbled the register space above
// 200 until the p201/p301 init loops repaired their own windows - and because
// mbPV is a per-instance map, registers 100-167 of the new instance stayed
// ABSENT entirely. setup() runs once per boot today, but the drivers-from-store
// replace-live path re-instantiates inverter drivers at runtime,
// where a second instance presents a garbled identity block to the inverter.

namespace {

// mbPV is protected in ModbusInverterProtocol; a subclass may read it.
struct BydModbusProbe : BydModbusInverter {
  uint16_t reg(uint16_t addr) {
    auto it = mbPV.find(addr);
    return it == mbPV.end() ? 0 : it->second;
  }
};

TEST(BydModbusStaticCursorTest, SecondInstanceStillWritesSiMarkerAtReg100) {
  {
    BydModbusProbe first;
    first.setup();
    ASSERT_EQ(first.reg(100), 21321u) << "si_data[0] must open the block at reg 100 on the FIRST instance too";
  }

  BydModbusProbe second;
  second.setup();

  // The defect resumed the cursor at 168 (the six arrays total 68 words from
  // 100): regs 100-167 were never written on the second instance (absent from
  // its map), and battery_data[0]=16985 landed at reg 186 - inside the
  // 168-199 zone that no init loop repairs (p201 re-covers only 200-212), so
  // it stayed visible to the inverter. Reg 214 would not do as the probe: it is
  // volt_data[12]=0 in the shifted layout too, so an assert on it could never
  // bite. Layout derived by dumping the poisoned map.
  EXPECT_EQ(second.reg(100), 21321u) << "the write cursor survived the previous instantiation";
  EXPECT_EQ(second.reg(186), 0u) << "the identity strings scribbled the unrepaired 168-199 zone";
}

}  // namespace
