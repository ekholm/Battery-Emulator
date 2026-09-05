#ifndef _CAN_INIT_PLAN_H_
#define _CAN_INIT_PLAN_H_

#include "../../devboard/hal/hal.h"

/* Which of init_CAN()'s three CAN-FD blocks may run on THIS board.
 *
 * An add-on the board does not have is skipped and left inert rather than
 * treated as an incoherent pin map - the same rule the MCP2515 block follows.
 * The FD side cannot do it block by block, though: the first block creates the
 * SPI bus the two chip blocks dereference, so skipping it on its own would hand
 * them a null. That dependency is the whole reason this decision is a struct
 * computed in one place instead of three conditions written where they are
 * used - and it is why this lives in a header the host suite can reach, since
 * comm_can.cpp itself does not link there.
 */
struct CanFdInitPlan {
  bool bus = false;          // the shared SPI bus block may run
  bool first_chip = false;   // the MCP2517 on CS/INT may run
  bool second_chip = false;  // the MCP2517 on CS2/INT2 may run
};

/* `want_first` / `want_second` are "an interface for this chip is registered".
 * Presence is asked only for a chip the caller actually wants, because
 * pins_present() raises EVENT_GPIO_NOT_DEFINED for an absent pin, and a board
 * that declares no second chip must not be told its bus-2 pins are missing.
 */
inline CanFdInitPlan plan_canfd_init(Esp32Hal* hal, bool want_first, bool want_second) {
  CanFdInitPlan plan;

  /* The first bus is asked about only for a chip that will actually use it: the
   * first one, or a second one that shares it. A board that registers neither,
   * or that registers only a second chip on its OWN bus, must not be told the
   * first bus's pins are missing - pins_present() raises EVENT_GPIO_NOT_DEFINED,
   * and that is an event about an interface nobody asked for. Same rule as the
   * CS2/INT2 check below, applied to the bus. */
  if (want_first || (want_second && hal->MCP2517_BUS() == hal->MCP2517_BUS2())) {
    plan.bus = hal->pins_present("CANFD", hal->MCP2517_SCK(), hal->MCP2517_SDO(), hal->MCP2517_SDI());
  }
  plan.first_chip = want_first && plan.bus && hal->pins_present("CANFD", hal->MCP2517_CS(), hal->MCP2517_INT());

  if (want_second && hal->pins_present("CANFD2", hal->MCP2517_CS2(), hal->MCP2517_INT2())) {
    if (hal->MCP2517_BUS() == hal->MCP2517_BUS2()) {
      /* Sharing the first chip's bus means copying that SPI pointer, so the
       * second chip is only usable if the first block actually ran. */
      plan.second_chip = plan.bus;
    } else {
      plan.second_chip = hal->pins_present("CANFD2", hal->MCP2517_SCK2(), hal->MCP2517_SDO2(), hal->MCP2517_SDI2());
    }
  }

  return plan;
}

#endif  // _CAN_INIT_PLAN_H_
