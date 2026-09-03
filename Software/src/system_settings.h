#ifndef SYSTEM_SETTINGS_H_
#define SYSTEM_SETTINGS_H_
/** TASKS
 * Higher number equals higher priority. Max 25 per core
 * 
 * Parameter: TASK_CORE_PRIO
 * Description:
 * Defines the priority of core functionality (CAN, Modbus, etc)
 * 
 * Parameter: TASK_CONNECTIVITY_PRIO
 * Description:
 * Defines the priority of various wireless functionality (TCP, MQTT, etc)
 * 
 * Parameter: TASK_MODBUS_PRIO
 * Description:
 * Defines the priority of MODBUS handling
 *
 * Parameter: TASK_ACAN2515_PRIORITY
 * Description:
 * Defines the priority of ACAN2515 CAN handling
 *  
 * Parameter: TASK_ACAN2515_PRIORITY
 * Description:
 * Defines the priority of ACAN2517FD CAN-FD handling
*/
#define TASK_CORE_PRIO 4
#define TASK_CONNECTIVITY_PRIO 3
#define TASK_MQTT_PRIO 2
#define TASK_MODBUS_PRIO 8
#define TASK_ACAN2515_PRIORITY 10
#define TASK_ACAN2517FD_PRIORITY 10

/** MAX AMOUNT OF CELLS
 * 
 * Parameter: MAX_AMOUNT_CELLS
 * Description:
 * Basically the length of the array used to hold individual cell voltages
*/
#define MAX_AMOUNT_CELLS 192

// AsyncTCP should only need a 4k stack (default is 16k).
// The second name is not redundant: AsyncTCP.h defines CONFIG_ASYNC_TCP_STACK_SIZE
// itself if nothing got there first, so comparing that macro against itself would
// pass at the library's 16384 too. BE_ASYNC_TCP_STACK_SIZE is set only here, which
// is what lets AsyncTCP.h's guard tell "the project's value won" from "the
// library's fallback won".
#define BE_ASYNC_TCP_STACK_SIZE 4096
#define CONFIG_ASYNC_TCP_STACK_SIZE BE_ASYNC_TCP_STACK_SIZE

#endif
