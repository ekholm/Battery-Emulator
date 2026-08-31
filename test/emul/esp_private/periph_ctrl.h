#pragma once

// Emulated stand-in for the IDF peripheral-clock control used to reset TWAI
// before (re)initialising the native CAN controller.
typedef enum { PERIPH_TWAI_MODULE } periph_module_t;

void periph_module_reset(periph_module_t periph);
