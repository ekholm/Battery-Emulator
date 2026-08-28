#ifndef PIN_EXCLUSIONS_H
#define PIN_EXCLUSIONS_H

#include "../../communication/equipmentstopbutton/comm_equipmentstopbutton.h"
#include "../../inverter/InverterProtocol.h"
#include "../hal/hal.h"

/* Board pin tables multiplex one GPIO across several roles on purpose: most
   coexisting roles are user-selectable alternatives or structurally exclusive
   options, so uniqueness cannot be asserted - it is red on every board on day
   one. What CAN be said is which role pairs must never be ACTIVE at the same
   time, and the settings layer refuses those combinations at selection time,
   before anything is stored - today the collision only surfaces at boot, as
   EVENT_GPIO_CONFLICT out of alloc_pins(), when the user can no longer act on
   it.

   The known shared-pin groups, and why they are legal (documented here so the
   next exclusion lands as a table entry, not an investigation):
   - Stark GPIO25: PRECHARGE / BMS_POWER swap under GPIOOPT5 (one setting picks
     which), INVERTER_DISCONNECT_CONTACTOR and WUP_PIN1 belong to features that
     are not enabled together with automatic precharging on this board.
   - Stark GPIO19: SECOND_BATTERY_CONTACTORS vs HIA4V1 - a second battery and
     automatic precharging are alternative installations.
   - Stark GPIO32: POSITIVE_CONTACTOR vs WUP_PIN2 - contactor control and
     battery wake-up are alternative wirings.
   - LilyGo GPIO18: MCP2515_CS / MCP2517_CS / CHADEMO_LOCK / BMS_POWER - the
     add-on header carries exactly one of these at a time.

   ENFORCED as mutually exclusive (each entry carries its ruling):
   - SMA inverter contactor enable x equipment stop button, where the board
     puts both on one pin (Stark: GPIO2). User ruling 2026-08-28. */

/* True when this inverter protocol drives the SMA contactor-enable pin
   (the SmaInverterBase subclasses; SMA LV is not one of them). */
bool inverter_uses_sma_contactor_pin(InverterProtocolType type);

/* nullptr when the would-be selection is legal; otherwise a static,
   user-showable sentence naming the conflict. Pure function of the candidate
   selection and the two pins, so hosts can test every board's table. */
const char* pin_exclusion_conflict(InverterProtocolType inverter, STOP_BUTTON_BEHAVIOR equipment_stop,
                                   gpio_num_t sma_contactor_pin, gpio_num_t equipment_stop_pin);

/* The same check against the running board's HAL. */
const char* pin_exclusion_conflict(InverterProtocolType inverter, STOP_BUTTON_BEHAVIOR equipment_stop);

#endif  // PIN_EXCLUSIONS_H
