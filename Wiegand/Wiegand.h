/*
 * Wiegand.h
 *
 *  Created on: Jan 28, 2025
 *      Author: Xurshid Xujamatov
 */

#ifndef WIEGAND_H_
#define WIEGAND_H_

#include "stm32f4xx_hal.h"
#include "stdbool.h"

// WIEGAND structure definition
typedef struct WIEGAND {
	volatile uint32_t cardTempHighHigh;
    volatile uint32_t cardTempHigh;
    volatile uint32_t cardTemp;
    volatile uint32_t lastWiegand;
    volatile int bitCount;
    int wiegandType;
    uint64_t code;
} WIEGAND;

// Function prototypes
void Wiegand_Init(WIEGAND* wg);
void Wiegand_ReadD0(WIEGAND* wg);
void Wiegand_ReadD1(WIEGAND* wg);
bool Wiegand_Available(WIEGAND* wg);
uint64_t Wiegand_GetCode(WIEGAND* wg);
int Wiegand_GetWiegandType(WIEGAND* wg);

#endif /* WIEGAND_H_ */
