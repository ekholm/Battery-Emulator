#include "BATTERIES.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/hal/hal.h"
#include "../devboard/utils/logging.h"
#include "CanBattery.h"
#include "RS485Battery.h"

#include "../shunt/BMW-SBOX.h"
#include "BMW-I3-BATTERY.h"
#include "BMW-IX-BATTERY.h"
#include "BMW-PHEV-BATTERY.h"
#include "BOLT-AMPERA-BATTERY.h"
#include "BYD-ATTO-3-BATTERY.h"
#include "CELLPOWER-BMS.h"
#include "CHADEMO-BATTERY.h"
#include "CHADEMO-CT.h"
#include "CHADEMO-SHUNTS.h"
#include "CHARGEBYTE-CCS.h"
#include "CMFA-EV-BATTERY.h"
#include "CMP-SMART-CAR-BATTERY.h"
#include "DALY-BMS.h"
#include "ECMP-BATTERY.h"
#include "ENNOID-BMS.h"
#include "FORD-MACH-E-BATTERY.h"
#include "FOXESS-BATTERY.h"
#include "GEELY-GEOMETRY-C-BATTERY.h"
#include "GEELY-SEA-BATTERY.h"
#include "GROWATT-HV-ARK-BATTERY.h"
#include "HYUNDAI-IONIQ-28-BATTERY.h"
#include "IMIEV-CZERO-ION-BATTERY.h"
#include "JAGUAR-IPACE-BATTERY.h"
#include "KIA-64FD-BATTERY.h"
#include "KIA-E-GMP-BATTERY.h"
#include "KIA-HYUNDAI-64-BATTERY.h"
#include "KIA-HYUNDAI-HYBRID-BATTERY.h"
#include "MEB-BATTERY.h"
#include "MG-5-BATTERY.h"
#include "MG-GEN1-BATTERY.h"
#include "NISSAN-LEAF-BATTERY.h"
#include "ORION-BMS.h"
#include "PYLON-BATTERY.h"
#include "RANGE-ROVER-PHEV-BATTERY.h"
#include "RELION-LV-BATTERY.h"
#include "RENAULT-KANGOO-BATTERY.h"
#include "RENAULT-TWIZY.h"
#include "RENAULT-ZOE-GEN1-BATTERY.h"
#include "RENAULT-ZOE-GEN2-BATTERY.h"
#include "RIVIAN-BATTERY.h"
#include "RJXZS-BMS.h"
#include "SAMSUNG-SDI-LV-BATTERY.h"
#include "SANTA-FE-PHEV-BATTERY.h"
#include "SIMPBMS-BATTERY.h"
#include "SONO-BATTERY.h"
#include "STELLANTIS-SMALL-WIDE-4x4.h"
#include "TESLA-BATTERY.h"
#include "TESLA-LEGACY-BATTERY.h"
#include "TEST-FAKE-BATTERY.h"
#include "THINK-BATTERY.h"
#include "THUNDERSTRUCK-BMS.h"
#include "VOLVO-SPA-BATTERY.h"
#include "VOLVO-SPA-HYBRID-BATTERY.h"

Battery* batteries[MAX_BATTERIES] = {};  // value-initialized: all null for any MAX_BATTERIES

std::vector<BatteryType> supported_battery_types() {
  std::vector<BatteryType> types;

  for (int i = 0; i < (int)BatteryType::Highest; i++) {
    types.push_back((BatteryType)i);
  }

  return types;
}

const char* name_for_chemistry(battery_chemistry_enum chem) {
  switch (chem) {
    case battery_chemistry_enum::Autodetect:
      return "Autodetect";
    case battery_chemistry_enum::LFP:
      return "LFP";
    case battery_chemistry_enum::NCA:
      return "NCA";
    case battery_chemistry_enum::NMC:
      return "NMC";
    case battery_chemistry_enum::ZEBRA:
      return "Molten Salt";
    default:
      return nullptr;
  }
}

const char* name_for_comm_interface(comm_interface comm) {
  return esp32hal->name_for_comm_interface(comm);
}

const char* name_for_battery_type(BatteryType type) {
  switch (type) {
    case BatteryType::None:
      return "None";
    case BatteryType::BmwI3:
      return BmwI3Battery::Name;
    case BatteryType::BmwIX:
      return BmwIXBattery::Name;
    case BatteryType::BmwPhev:
      return BmwPhevBattery::Name;
    case BatteryType::BoltAmpera:
      return BoltAmperaBattery::Name;
    case BatteryType::BydAtto3:
      return BydAttoBattery::Name;
    case BatteryType::CellPowerBms:
      return CellPowerBms::Name;
    case BatteryType::Chademo:
      return ChademoBattery::Name;
    case BatteryType::CmfaEv:
      return CmfaEvBattery::Name;
    case BatteryType::CmpSmartCar:
      return CmpSmartCarBattery::Name;
    case BatteryType::EnnoidBMS:
      return EnnoidBms::Name;
    case BatteryType::FordMachE:
      return FordMachEBattery::Name;
    case BatteryType::Foxess:
      return FoxessBattery::Name;
    case BatteryType::GeelyGeometryC:
      return GeelyGeometryCBattery::Name;
    case BatteryType::GrowattHvArk:
      return GrowattHvArkBattery::Name;
    case BatteryType::HyundaiIoniq28:
      return HyundaiIoniq28Battery::Name;
    case BatteryType::OrionBms:
      return OrionBms::Name;
    case BatteryType::Sono:
      return SonoBattery::Name;
    case BatteryType::StellantisEcmp:
      return EcmpBattery::Name;
    case BatteryType::ImievCZeroIon:
      return ImievCZeroIonBattery::Name;
    case BatteryType::JaguarIpace:
      return JaguarIpaceBattery::Name;
    case BatteryType::KiaEGmp:
      return KiaEGmpBattery::Name;
    case BatteryType::KiaHyundai64:
      return KiaHyundai64Battery::Name;
    case BatteryType::Kia64FD:
      return Kia64FDBattery::Name;
    case BatteryType::KiaHyundaiHybrid:
      return KiaHyundaiHybridBattery::Name;
    case BatteryType::Meb:
      return MebBattery::Name;
    case BatteryType::VAGMqbEvo:
      return MqbEvoBattery::Name;
#ifndef SMALL_FLASH_DEVICE
    case BatteryType::Mg5:
      return Mg5Battery::Name;
#endif
    case BatteryType::MgGen1:
      return MgGen1Battery::Name;
    case BatteryType::NissanLeaf:
      return NissanLeafBattery::Name;
    case BatteryType::Pylon:
      return PylonBattery::Name;
    case BatteryType::DalyBms:
      return DalyBms::Name;
    case BatteryType::RjxzsBms:
      return RjxzsBms::Name;
    case BatteryType::RangeRoverPhev:
      return RangeRoverPhevBattery::Name;
    case BatteryType::RelionBattery:
      return RelionBattery::Name;
    case BatteryType::RenaultKangoo:
      return RenaultKangooBattery::Name;
    case BatteryType::RenaultTwizy:
      return RenaultTwizyBattery::Name;
    case BatteryType::RenaultZoe1:
      return RenaultZoeGen1Battery::Name;
    case BatteryType::RenaultZoe2:
      return RenaultZoeGen2Battery::Name;
    case BatteryType::RivianBattery:
      return RivianBattery::Name;
    case BatteryType::SamsungSdiLv:
      return SamsungSdiLVBattery::Name;
    case BatteryType::SantaFePhev:
      return SantaFePhevBattery::Name;
    case BatteryType::StellantisSmallWide4x4:
      return StellantisSmallWide4x4Battery::Name;
    case BatteryType::SimpBms:
      return SimpBmsBattery::Name;
    case BatteryType::TeslaModel3Y:
      return TeslaBattery::Name3Y;
    case BatteryType::TeslaModelSX:
      return TeslaBattery::NameSX;
    case BatteryType::TeslaLegacy:
      return TeslaLegacyBattery::Name;
    case BatteryType::TestFake:
      return TestFakeBattery::Name;
    case BatteryType::ThinkCity:
      return ThinkBattery::Name;
    case BatteryType::ThunderstruckBMS:
      return ThunderstruckBMS::Name;
    case BatteryType::GeelySea:
      return GeelySeaBattery::Name;
    case BatteryType::VolvoSpa:
      return VolvoSpaBattery::Name;
    case BatteryType::VolvoSpaHybrid:
      return VolvoSpaHybridBattery::Name;
#ifndef SMALL_FLASH_DEVICE
    case BatteryType::ChargebyteCCSBattery:
      return ChargebyteCCSBattery::Name;
#endif
    default:
      return nullptr;
  }
}

const battery_chemistry_enum battery_chemistry_default = battery_chemistry_enum::NMC;

battery_chemistry_enum user_selected_battery_chemistry = battery_chemistry_default;

BatteryType user_selected_battery_type = BatteryType::None;
bool user_selected_second_battery = false;
bool user_selected_triple_battery = false;

// One construction switch for every instance. instance 0 = primary (the
// default-constructor form, wired to the primary globals); instance 1..2 =
// extra batteries via the injected-instance constructors. Types without a
// secondary form return nullptr for instance > 0.
Battery* create_battery(BatteryType type, int instance) {
  // Every driver has ONE constructor with defaulted parameters, so the
  // primary is just instance 0's arguments: its own datalayer entry and the
  // primary CAN interface. No per-case primary/secondary split remains.
  DATALAYER_BATTERY_TYPE* dl = &datalayer_battery(instance);
  CAN_Interface can = can_config.batteries[instance];
  // Core-permits flag for this instance (primary constructions ignore it -
  // their merged ctors derive the primary wiring).
  bool* acc = &datalayer.system.status.battery_link[instance].allowed_contactor_closing;
  (void)acc;
  switch (type) {
    case BatteryType::None:
      return nullptr;
    case BatteryType::BmwI3:
      // The wakeup pin is per-instance and the boards provide exactly two -
      // a third instance has no pin, so it fails closed here as well as at
      // the battery_supports_triple() gate.
      return instance <= 1 ? new BmwI3Battery(dl, acc, can, instance == 0 ? esp32hal->WUP_PIN1() : esp32hal->WUP_PIN2())
                           : nullptr;
    case BatteryType::BmwIX:
      return new BmwIXBattery(dl, can);
    case BatteryType::BmwPhev:
      return new BmwPhevBattery(dl, can);
    case BatteryType::BoltAmpera:
      return new BoltAmperaBattery(dl, can);
    case BatteryType::BydAtto3:
      // Only two extended-data instances exist (bydAtto3, bydAtto3_2) - a
      // third battery would alias the second's, so it fails closed here as
      // well as at the battery_supports_triple() gate.
      return instance <= 1 ? new BydAttoBattery(dl, &dl->extended.bydAtto3, can) : nullptr;
    case BatteryType::CellPowerBms:
      return new CellPowerBms(dl, can);
    case BatteryType::Chademo:
      return new ChademoBattery(dl, can);
    case BatteryType::CmfaEv:
      return new CmfaEvBattery(dl, can);
    case BatteryType::CmpSmartCar:
      return new CmpSmartCarBattery(dl, can);
    case BatteryType::EnnoidBMS:
      return new EnnoidBms(dl, can);
    case BatteryType::FordMachE:
      return new FordMachEBattery(dl, can);
    case BatteryType::Foxess:
      return new FoxessBattery(dl, can);
    case BatteryType::GeelyGeometryC:
      return new GeelyGeometryCBattery(dl, can);
    case BatteryType::GrowattHvArk:
      return new GrowattHvArkBattery(dl, can);
    case BatteryType::HyundaiIoniq28:
      return new HyundaiIoniq28Battery(dl, can);
    case BatteryType::OrionBms:
      return new OrionBms(dl, can);
    case BatteryType::Sono:
      return new SonoBattery(dl, can);
    case BatteryType::StellantisEcmp:
      return new EcmpBattery(dl, can);
    case BatteryType::ImievCZeroIon:
      return new ImievCZeroIonBattery(dl, can);
    case BatteryType::JaguarIpace:
      return new JaguarIpaceBattery(dl, can);
    case BatteryType::Kia64FD:
      return new Kia64FDBattery(dl, can);
    case BatteryType::KiaEGmp:
      return new KiaEGmpBattery(dl, can);
    case BatteryType::KiaHyundai64:
      return new KiaHyundai64Battery(dl, acc, can);
    case BatteryType::KiaHyundaiHybrid:
      return new KiaHyundaiHybridBattery(dl, can);
    case BatteryType::Meb:
      return new MebBattery(dl, can);
    case BatteryType::VAGMqbEvo:
      return new MqbEvoBattery(dl, can);
#ifndef SMALL_FLASH_DEVICE
    case BatteryType::Mg5:
      return new Mg5Battery(dl, can);
#endif
    case BatteryType::MgGen1:
      return new MgGen1Battery(dl, can, acc);
    case BatteryType::NissanLeaf:
      return new NissanLeafBattery(dl, can);
    case BatteryType::Pylon:
      return new PylonBattery(dl, nullptr, can);
    case BatteryType::DalyBms:
      return instance == 0 ? new DalyBms() : nullptr;
    case BatteryType::RjxzsBms:
      return new RjxzsBms(dl, can);
    case BatteryType::RangeRoverPhev:
      return new RangeRoverPhevBattery(dl, can);
    case BatteryType::RelionBattery:
      return new RelionBattery(dl, can, acc);
    case BatteryType::RenaultKangoo:
      return new RenaultKangooBattery(dl, can);
    case BatteryType::RenaultTwizy:
      return new RenaultTwizyBattery(dl, can);
    case BatteryType::RenaultZoe1:
      return new RenaultZoeGen1Battery(dl, can);
    case BatteryType::RenaultZoe2:
      return new RenaultZoeGen2Battery(dl, can);
    case BatteryType::RivianBattery:
      return new RivianBattery(dl, can);
    case BatteryType::SamsungSdiLv:
      return new SamsungSdiLVBattery(dl, can);
    case BatteryType::SantaFePhev:
      return new SantaFePhevBattery(dl, can);
    case BatteryType::SimpBms:
      return new SimpBmsBattery(dl, can);
    case BatteryType::StellantisSmallWide4x4:
      return new StellantisSmallWide4x4Battery(dl, can);
    case BatteryType::TeslaModel3Y:
    case BatteryType::TeslaModelSX:
      return new TeslaBattery(dl, can);
    case BatteryType::TeslaLegacy:
      return new TeslaLegacyBattery(dl, can);
    case BatteryType::TestFake:
      return new TestFakeBattery(dl, can);
    case BatteryType::ThinkCity:
      return new ThinkBattery(dl, can);
    case BatteryType::ThunderstruckBMS:
      return new ThunderstruckBMS(dl, can);
    case BatteryType::GeelySea:
      return new GeelySeaBattery(dl, can);
    case BatteryType::VolvoSpa:
      return new VolvoSpaBattery(dl, can);
    case BatteryType::VolvoSpaHybrid:
      return new VolvoSpaHybridBattery(dl, nullptr, can);
#ifndef SMALL_FLASH_DEVICE
    case BatteryType::ChargebyteCCSBattery:
      return new ChargebyteCCSBattery(dl, can);
#endif
    default:
      return nullptr;
  }
}

// The integrations that can be instantiated a second time on a separate
// interface. Must match the switch in setup_battery() below.
// Extra-instance support is opt-OUT: with instance-injected datalayer access
// and instance-owned extended data, any stateless driver can run more than
// once. The exceptions carry their reason; entries marked "pending de-static"
// unlock when the driver sheds its file-scope/function-local mutable state.
static bool battery_supports_extra_instances(BatteryType type) {
  switch (type) {
    case BatteryType::None:
      return false;
    case BatteryType::Chademo:               // singleton shunt/CT hardware + heavy static state
    case BatteryType::JaguarIpace:           // pending de-static (32 file-scope statics)
    case BatteryType::DalyBms:               // single RS485 transport
    case BatteryType::TeslaLegacy:           // pending de-static (incl. shared buffers)
    case BatteryType::BmwPhev:               // pending de-static
    case BatteryType::ChargebyteCCSBattery:  // pending de-static
    case BatteryType::GrowattHvArk:          // pending de-static
      return false;
    default:
      return true;
  }
}

bool battery_supports_double(BatteryType type) {
  return battery_supports_extra_instances(type);
}

bool battery_supports_triple(BatteryType type) {
  // BmwI3 needs a per-instance wakeup pin and the boards provide two;
  // BydAtto3 keeps a core-coupled extended struct with only a double variant.
  return battery_supports_extra_instances(type) && type != BatteryType::BmwI3 && type != BatteryType::BydAtto3;
}

// The integrations that can be instantiated a third time on a separate
// interface. Must match the switch in setup_battery() below.

void setup_battery() {
  if (batteries[0]) {
    // Let's not create the battery again.
    return;
  }

  // Set the chemistry to the user selected value, the battery can override.
  for (int i = 0; i < MAX_BATTERIES; ++i) {
    datalayer_battery(i).info.chemistry = user_selected_battery_chemistry;
  }

  // The reference pack needs no parallel-join permission - mark it allowed.
  datalayer.system.status.battery_link[0].allowed_contactor_closing = true;

  // One loop constructs every requested instance. The primary has no
  // request flag and no capability gate; the extras have both.
  const struct {
    bool requested;
    bool (*supported)(BatteryType);
    static_assert(MAX_BATTERIES == 3, "add the new instance's request flag and capability gate");
  } wanted[MAX_BATTERIES] = {
      {true, nullptr},
      {user_selected_second_battery, battery_supports_double},
      {user_selected_triple_battery, battery_supports_triple},
  };
  for (int i = 0; i < MAX_BATTERIES; ++i) {
    const auto& want = wanted[i];
    if (!want.requested || batteries[i]) {
      continue;
    }
    if (want.supported && !want.supported(user_selected_battery_type)) {
      DEBUG_PRINTF("User tried enabling %s battery on non-supported integration!\n", Battery::instance_label[i]);
      continue;
    }
    batteries[i] = create_battery(user_selected_battery_type, i);
    if (batteries[i]) {
      // Per-pack events resolve through the index (upstream #2799); the loop
      // is this branch's replacement for the twins that carried it.
      batteries[i]->battery_index = i + 1;
      batteries[i]->setup();
    }
  }
}

/* User-selected Nissan LEAF settings */
bool user_selected_LEAF_interlock_mandatory = false;
/* User-selected Tesla settings */
bool user_selected_tesla_digital_HVIL = false;
uint16_t user_selected_tesla_GTW_country = 17477;
bool user_selected_tesla_GTW_rightHandDrive = true;
uint16_t user_selected_tesla_GTW_mapRegion = 2;
uint16_t user_selected_tesla_GTW_chassisType = 2;
uint16_t user_selected_tesla_GTW_packEnergy = 1;
/* User-selected DALY BMS settings */
int user_selected_daly_power_per_percent = 50;
int user_selected_daly_power_per_dV = 50;
int user_selected_daly_power_per_dV_start = 20;
int user_selected_daly_power_per_degree_C = 60;
int user_selected_daly_power_at_0_degree_C = 800;
/* User-selected EGMP+others settings */
bool user_selected_use_estimated_SOC = false;
bool user_selected_use_estimated_charge_limits = false;
uint16_t user_selected_pylon_baudrate = 500;
// Use 0V for user selected cell/pack voltage defaults (On boot will be replaced with saved values from NVM)
uint16_t user_selected_max_pack_voltage_dV = 0;
uint16_t user_selected_min_pack_voltage_dV = 0;
uint16_t user_selected_max_cell_voltage_mV = 0;
uint16_t user_selected_min_cell_voltage_mV = 0;
