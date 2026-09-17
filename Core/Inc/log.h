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
#include "stdbool.h"
#include "stm32f4xx_hal.h"

/* USART1 TX halqa buferi. 2 ning darajasi bo'lishi SHART (mask bilan wrap). */
#define LOG_TX_BUF_SIZE 2048

/*
 * Loggerni ishga tushiradi: USART1 ni (PA9, 115200 8N1, faqat TX) va
 * DMA2_Stream7 ni sozlaydi, stdout ni statik buferli satrli rejimga o'tkazadi.
 *
 * USART1 CubeMX da sozlanmagan - uni butunlay shu modul ko'taradi, shuning
 * uchun SystemClock_Config() dan KEYIN va birinchi printf dan OLDIN
 * chaqirilishi shart (BRR PCLK2 dan hisoblanadi).
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

/*
 * Debug chiqishini yoqadi / o'chiradi (config QR i orqali boshqariladi).
 * O'chirilganda printf jimgina tashlanadi.
 * Log_Panic() bunga bo'ysunmaydi - u o'zining alohida yo'li bilan yozadi,
 * shuning uchun avariya xabari debug o'chiq bo'lsa ham yo'qolmaydi.
 */
void Log_SetEnabled(bool enabled);

/* Bufer to'lib qolgani sababli tashlab yuborilgan baytlarning umumiy soni. */
uint32_t Log_DroppedBytes(void);

/*
 * Qaytmaydigan fatal xato signali.
 *
 * Buferdagi qoldiqni va msg ni DMA/uzilishlarsiz, to'g'ridan-to'g'ri USART1
 * registriga yozib chiqaradi (USART1 kloki yoqilgan bo'lsa), keyin LED larni
 * O'ZI sozlab, blinks marta miltillab cheksiz takrorlaydi.
 *
 * LED larni o'zi sozlashi muhim: Error_Handler() SystemClock_Config() dan ham
 * chaqirilishi mumkin, u paytda MX_GPIO_Init() hali ishlamagan va xato
 * butunlay jimgina yuz berardi - na LED, na UART.
 */
void Log_Panic(const char *msg, uint8_t blinks);

/*
 * Diagnostika belgisi: LED2 ni n marta miltillatadi va pauza qiladi.
 *
 * HAL taymeriga bog'liq emas (HAL_Init() dan oldin ham ishlaydi) va GPIOE
 * klokini o'zi yoqadi. Miltillash tezligi SystemCoreClock ga moslanadi,
 * shuning uchun HSI 16MHz da ham, PLL 168MHz da ham bir xil ko'rinadi.
 *
 * Boot bosqichlarini belgilash uchun ishlatiladi - qayerda to'xtaganini
 * oxirgi ko'ringan sanoqdan bilib olinadi. Diagnostika tugagach chaqiruvlarni
 * olib tashlash mumkin.
 */
void Boot_Mark(uint8_t blinks);

/* Miltillash kodlari - LED larni sanab qaysi xato ekanini aniqlash uchun */
#define LOG_PANIC_ERROR_HANDLER 2u  /* Error_Handler(): klok yoki periferiya init */
#define LOG_PANIC_HARDFAULT     3u
#define LOG_PANIC_MEMFAULT      4u
#define LOG_PANIC_BUSFAULT      5u
#define LOG_PANIC_USAGEFAULT    6u

#define LOG_INFO(tag, fmt, ...) printf("[%08lu][INFO][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)
#define LOG_OK(tag, fmt, ...)   printf("[%08lu][ OK ][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)
#define LOG_XATO(tag, fmt, ...) printf("[%08lu][XATO][" tag "] " fmt "\n", HAL_GetTick(), ##__VA_ARGS__)

#endif /* LOG_H_ */
