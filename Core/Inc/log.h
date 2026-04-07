/*
 * log.h - Structured logging macros
 *
 * Format: [timestamp][LEVEL][TAG] message
 *
 * Created on: 2025
 * Author: Xurshid Xujamatov
 */

#ifndef LOG_H_
#define LOG_H_

#include "stdio.h"
#include "stm32f4xx_hal.h"

#define LOG_INFO(tag, fmt, ...) printf("[%08lu][INFO][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)
#define LOG_OK(tag, fmt, ...)   printf("[%08lu][ OK ][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)
#define LOG_XATO(tag, fmt, ...) printf("[%08lu][XATO][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)

#endif /* LOG_H_ */
