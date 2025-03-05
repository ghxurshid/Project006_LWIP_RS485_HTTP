/*
 * Wiegand.cpp
 *
 *  Created on: Jan 28, 2025
 *      Author: Admin
 */

#include "Wiegand.h"

static bool Wiegand_DoConversion(WIEGAND* wg);
static uint64_t Wiegand_GetCardId(volatile uint32_t* codeHighHigh, volatile uint32_t* codeHigh, volatile uint32_t* codeLow, uint8_t bitLength);

uint8_t p = 0;
uint8_t n = 0;

// Initialize WIEGAND structure
void Wiegand_Init(WIEGAND* wg/*, GPIO_TypeDef* portD0, uint16_t pinD0, GPIO_TypeDef* portD1, uint16_t pinD1*/) {
//    wg->portD0 = portD0;
//    wg->pinD0 = pinD0;
//    wg->portD1 = portD1;
//    wg->pinD1 = pinD1;
    wg->cardTempHigh = 0;
    wg->cardTemp = 0;
    wg->lastWiegand = 0;
    wg->bitCount = 0;
    wg->wiegandType = 0;
    wg->code = 0;
    wg->busy = 0;
}

// Simulate D0 signal read
void Wiegand_ReadD0(WIEGAND* wg) {
	wg->busy = 1;
    wg->bitCount++;

    if (wg->bitCount > 64)
    {
    	wg->cardTempHighHigh <<= 1;
    	wg->cardTempHighHigh |= ((wg->cardTempHigh & 0x8000000000000000) >> 63);
    }

	if (wg->bitCount > 31)
	{
		wg->cardTempHigh <<= 1;
		wg->cardTempHigh |= ((wg->cardTemp & 0x80000000) >> 31);
	}

	wg->cardTemp <<= 1;
	wg->cardTemp &= 0xFFFFFFFE;

    wg->lastWiegand = HAL_GetTick();
    wg->busy = 0;
    n ++;
}

// Simulate D1 signal read
void Wiegand_ReadD1(WIEGAND* wg) {
	wg->busy = 1;
    wg->bitCount++;

    if (wg->bitCount > 64)
    {
    	wg->cardTempHighHigh <<= 1;
    	wg->cardTempHighHigh |= ((wg->cardTempHigh & 0x8000000000000000) >> 63);
    }

	if (wg->bitCount > 31)
	{
		wg->cardTempHigh <<= 1;
		wg->cardTempHigh |= ((wg->cardTemp & 0x80000000) >> 31);
	}

	wg->cardTemp <<= 1;
	wg->cardTemp |= 1;

    wg->lastWiegand = HAL_GetTick();
    wg->busy = 0;
    p ++;
}

// Check if a Wiegand code is available
bool Wiegand_Available(WIEGAND* wg) {
	if (wg->busy == 0)
	{
		return Wiegand_DoConversion(wg);
	}
	return false;
}

// Get the received Wiegand code
uint32_t Wiegand_GetCode(WIEGAND* wg) {
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
        if ((wg->bitCount == 24) || (wg->bitCount == 26) || (wg->bitCount == 32) || (wg->bitCount == 34) || (wg->bitCount == 66)) {
            wg->code = Wiegand_GetCardId(&wg->cardTempHighHigh, &wg->cardTempHigh, &wg->cardTemp, wg->bitCount);
            wg->wiegandType = wg->bitCount;

            // Reset bit count and temp storage
            wg->bitCount = 0;
            wg->cardTemp = 0;
            wg->cardTempHigh = 0;
            wg->cardTempHighHigh = 0;
            return true;
        } else {
            // Reset the state if no valid bit count is detected
            wg->lastWiegand = sysTick;
            wg->bitCount = 0;
            wg->cardTemp = 0;
            wg->cardTempHigh = 0;
            wg->cardTempHighHigh = 0;
        }
    }
    return false;
}

// Extract the card ID from the Wiegand data
static uint64_t Wiegand_GetCardId(volatile uint32_t* codeHighHigh, volatile uint32_t* codeHigh, volatile uint32_t* codeLow, uint8_t bitLength) {
    if (bitLength == 26)
    {
        return ((*codeLow >> 1) & 0xFFFFF);
    }
    else if (bitLength == 34)
    {
        *codeHigh = *codeHigh & 0x03;
        *codeHigh <<= 31;
        *codeLow >>= 1;
        return *codeHigh | *codeLow;
    }
    else if (bitLength == 66)
    {
    	*codeLow = ((*codeHigh & 0x01) << 31) | (*codeLow >> 1);
    	*codeHigh = (*codeHigh >> 1) | ((*codeHighHigh & 0x01) << 31);

		return (uint64_t)(*codeHigh) | (uint64_t)(*codeLow);
    }
    return (uint64_t)*codeLow;
}


