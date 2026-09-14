#ifndef _CAN_RX_RING_DEPTH_H_
#define _CAN_RX_RING_DEPTH_H_

#include <cstdint>

/* How many received frames each driver holds while nothing is taking them.
 *
 * The ring is what carries a bus through a flash write. While an OTA writes,
 * the loop that drains these rings stops for tens of milliseconds at a time.
 * Frames go on arriving, so the ring has to hold every frame that lands in the
 * longest stall. A path therefore stays lossless while
 *
 *     depth / offered rate > longest stall
 *
 * and that relation was predicted and then confirmed on the bench, on the
 * native and the CAN-FD paths independently.
 *
 * The drivers ship with 32, which covers 16 ms at 2,000 frames/s. The longest
 * single stall measured across an OTA upload was 85-94 ms on the ESP32-S3
 * boards. A classic ESP32 stalls about 71 ms once erases are 4 KB sectors
 * (CONFIG_SPI_FLASH_BYPASS_BLOCK_ERASE), and about 315 ms without that option.
 * A settings save stalls about 22 ms, which 32 already covers.
 *
 * Sized for 2,000 frames/s through a 94 ms stall: 188 frames, rounded up to 192.
 * The rounding gives 96 ms of cover at that rate. Put the other way, the
 * predicted knee is 2,042 frames/s against a 94 ms stall and 2,704 frames/s
 * against 71 ms. Both knees are predictions until the rig re-runs on this depth.
 *
 * What it costs, per interface, in heap (sizes are the drivers' own frame
 * records):
 *   native TWAI  192 x 16 B =  3,072 B  (was   512)  internal DRAM - an IRAM
 *                                                    interrupt writes it
 *   CAN-FD       192 x 72 B = 13,824 B  (was 2,304)  per MCP2518FD, allocated
 *                                                    once at begin()
 * The MCP2515 ring is sized separately (MCP2515_LITE_ISR_RING_DEPTH), because
 * that ring must be a power of two.
 */
constexpr uint16_t CAN_DRIVER_RX_RING_DEPTH = 192;

// The two figures the depth is sized against. They are kept as constants so a
// test can hold the depth to them.
constexpr uint32_t CAN_RX_RING_SIZED_RATE_FPS = 2000;
constexpr uint32_t CAN_RX_RING_SIZED_STALL_MS = 94;

#endif
