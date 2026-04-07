/*
 * Wiegand.cpp
 *
 *  Created on: Jan 28, 2025
 *      Author: Xurshid Xujamatov
 */

#include "Wiegand.h"

static bool Wiegand_DoConversion(WIEGAND* wg);
static uint64_t Wiegand_GetCardId(uint32_t codeHighHigh, uint32_t codeHigh, uint32_t codeLow, uint8_t bitLength);

// Initialize WIEGAND structure
void Wiegand_Init(WIEGAND* wg) {
    wg->cardTempHighHigh = 0;
    wg->cardTempHigh = 0;
    wg->cardTemp = 0;
    wg->lastWiegand = 0;
    wg->bitCount = 0;
    wg->wiegandType = 0;
    wg->code = 0;
}

// Simulate D0 signal read
void Wiegand_ReadD0(WIEGAND* wg) {
    wg->bitCount++;

    if (wg->bitCount > 64)
    {
    	wg->cardTempHighHigh <<= 1;
    	wg->cardTempHighHigh |= ((wg->cardTempHigh & 0x80000000) >> 31);
    }

	if (wg->bitCount > 31)
	{
		wg->cardTempHigh <<= 1;
		wg->cardTempHigh |= ((wg->cardTemp & 0x80000000) >> 31);
	}

	wg->cardTemp <<= 1;

    wg->lastWiegand = HAL_GetTick();
}

// Simulate D1 signal read
void Wiegand_ReadD1(WIEGAND* wg) {
    wg->bitCount++;

    if (wg->bitCount > 64)
    {
    	wg->cardTempHighHigh <<= 1;
    	wg->cardTempHighHigh |= ((wg->cardTempHigh & 0x80000000) >> 31);
    }

	if (wg->bitCount > 31)
	{
		wg->cardTempHigh <<= 1;
		wg->cardTempHigh |= ((wg->cardTemp & 0x80000000) >> 31);
	}

	wg->cardTemp <<= 1;
	wg->cardTemp |= 1;

    wg->lastWiegand = HAL_GetTick();
}

// Check if a Wiegand code is available
bool Wiegand_Available(WIEGAND* wg) {
    return Wiegand_DoConversion(wg);
}

// Get the received Wiegand code
uint64_t Wiegand_GetCode(WIEGAND* wg) {
    return wg->code;
}

// Get the Wiegand type
int Wiegand_GetWiegandType(WIEGAND* wg) {
    return wg->wiegandType;
}

// Perform Wiegand conversion logic
static bool Wiegand_DoConversion(WIEGAND* wg) {
    uint32_t sysTick = HAL_GetTick();
    if ((sysTick - wg->lastWiegand) > 25) {
        // Atomik o'qish — interrupt bilan race condition oldini olish
        __disable_irq();
        int bc = wg->bitCount;
        uint32_t tHH = wg->cardTempHighHigh;
        uint32_t tH  = wg->cardTempHigh;
        uint32_t tL  = wg->cardTemp;
        wg->bitCount = 0;
        wg->cardTemp = 0;
        wg->cardTempHigh = 0;
        wg->cardTempHighHigh = 0;
        __enable_irq();

        if ((bc == 24) || (bc == 26) || (bc == 32) || (bc == 34) || (bc == 66)) {
            wg->code = Wiegand_GetCardId(tHH, tH, tL, bc);
            wg->wiegandType = bc;
            return true;
        } else {
            wg->lastWiegand = sysTick;
        }
    }
    return false;
}

// Extract the card ID from the Wiegand data
static uint64_t Wiegand_GetCardId(uint32_t codeHighHigh, uint32_t codeHigh, uint32_t codeLow, uint8_t bitLength) {
    if (bitLength == 26)
    {
        // 1 parity + 8 facility + 16 card + 1 parity = 26 bit
        return ((codeLow >> 1) & 0xFFFFFF);
    }
    else if (bitLength == 34)
    {
        // 1 parity + 32 data + 1 parity — parity bitlarni olib tashlash
        uint64_t result = ((uint64_t)(codeHigh & 0x01) << 32) | codeLow;
        result >>= 1;          // oxirgi parity olib tashlash
        result &= 0xFFFFFFFF;  // 32-bit data
        return result;
    }
    else if (bitLength == 66)
    {
        // 1 parity + 64 data + 1 parity — parity bitlarni olib tashlash
        (void)codeHighHigh;
        uint64_t result = ((uint64_t)codeHigh << 32) | (uint64_t)codeLow;
        result >>= 1;  // oxirgi parity olib tashlash
        return result;
    }
    return (uint64_t)codeLow;
}


