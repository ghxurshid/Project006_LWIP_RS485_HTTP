/*
 * log.h - Structured logging macros + non-blocking DMA backend
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

/* USART1 TX halqa buferi. 2 ning darajasi bo'lishi SHART (mask bilan wrap). */
#define LOG_TX_BUF_SIZE 2048

/*
 * Loggerni ishga tushiradi: DMA2_Stream7 (USART1_TX) ni sozlaydi va stdout ni
 * statik buferli satrli rejimga o'tkazadi.
 * MX_USART1_UART_Init() ichida, HAL_UART_Init() dan keyin chaqiriladi -
 * shunda birinchi printf gacha logger tayyor bo'ladi.
 */
void Log_Init(void);

/*
 * Halqa buferga nusxa oladi va darhol qaytadi - hech qachon bloklamaydi.
 * ISR ichidan ham xavfsiz.
 * Xabar sig'masa BUTUNLAY tashlanadi (yarim satr chiqmaydi) va hisobga
 * olinadi; joy bo'shagach bo'shliq o'rniga bir marta belgi qo'yiladi.
 */
void Log_Write(const char *data, uint16_t len);

/* Bufer bo'shashini kutadi. Faqat reset/Error_Handler oldidan ishlatilsin. */
void Log_Flush(uint32_t timeout_ms);

/* Bufer to'lib qolgani sababli tashlab yuborilgan baytlarning umumiy soni. */
uint32_t Log_DroppedBytes(void);

#define LOG_INFO(tag, fmt, ...) printf("[%08lu][INFO][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)
#define LOG_OK(tag, fmt, ...)   printf("[%08lu][ OK ][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)
#define LOG_XATO(tag, fmt, ...) printf("[%08lu][XATO][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)

#endif /* LOG_H_ */
