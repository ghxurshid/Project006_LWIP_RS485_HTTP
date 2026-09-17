/*
 * log.h - Structured logging macros + bloklamaydigan halqa buferli backend
 *
 * Format: [timestamp][LEVEL][TAG] message
 *
 *   printf -> _write -> halqa bufer -> LogSink -> periferiya
 *
 * Bu modul PERIFERIYANI BILMAYDI. USART ni ham, DMA ni ham CubeMX sozlaydi,
 * tayyor handle esa Log_Init() ga LogSink orqali beriladi (dependency
 * injection). Shundan ikki foyda:
 *   - .ioc qayta generatsiya qilinganda modulga tegilmaydi va modul
 *     CubeMX sozlamalari ustiga hech narsa yozmaydi;
 *   - printf ni keyinchalik butunlay boshqa kanalga (USB CDC, TCP, boshqa
 *     UART) burish uchun faqat yangi sink yozish kifoya - log.c o'zgarmaydi.
 *
 * Tayyor sink: LogSinkUart_Create() - qarang log_sink_uart.h
 *
 * Created on: 2025
 * Author: Xurshid Xujamatov
 */

#ifndef LOG_H_
#define LOG_H_

#include "stdio.h"
#include "stdbool.h"
#include "stdint.h"
#include "stm32f4xx_hal.h"

/* USART TX halqa buferi. 2 ning darajasi bo'lishi SHART (mask bilan wrap). */
#define LOG_TX_BUF_SIZE 2048

/* ================= Chiqish kanali (sink) ================= */

/*
 * Logger bilan periferiya orasidagi yagona shartnoma.
 *
 * Struktura statik umrga ega bo'lishi SHART - logger unga ko'rsatkichni
 * saqlab qoladi va har uzatmada ishlatadi.
 */
typedef struct
{
    /*
     * Bo'lakni fonda yuborishni boshlaydi. BLOKLAMASLIGI SHART.
     *
     * Logger buni uzilishlar o'chirilgan holatda chaqiradi, shuning uchun
     * ichida HAL_Delay()/HAL_GetTick() ga tayangan kutish bo'lmasin.
     *
     * true  - uzatma boshlandi; tugagach Log_TxDone(), xato bo'lsa
     *         Log_TxFailed() chaqirilishi SHART, aks holda log to'xtaydi.
     * false - hozir boshlab bo'lmadi (band/xato); logger keyingi yozuvda
     *         qayta uradi, ma'lumot buferda qoladi.
     *
     * 'data' halqa buferning ichiga ko'rsatadi va uzatma tugagunicha
     * o'zgarmaydi. DMA ishlatilsa e'tibor bering: bufer .bss (SRAM) da,
     * CCMRAM da emas - DMA2 CCMRAM ni ko'ra olmaydi.
     */
    bool (*start)(void *ctx, const uint8_t *data, uint16_t len);

    /*
     * Fonda ketayotgan uzatmani to'xtatadi. Faqat Log_Panic() dan,
     * uzilishlar o'chirilgan holatda chaqiriladi - shuning uchun bu ham
     * taymerga tayanmasin (HAL_UART_Abort() YARAMAYDI).
     * NULL bo'lishi mumkin.
     */
    void (*abort)(void *ctx);

    /*
     * Bitta baytni DMA/uzilishlarsiz, to'g'ridan-to'g'ri registr orqali
     * yozadi. Faqat Log_Panic() ishlatadi. Chiqish hech qachon tayyor
     * bo'lmasa ham CHEKLANGAN vaqtda qaytishi SHART.
     * NULL bo'lsa panikda matn chiqmaydi, LED signali baribir ishlaydi.
     */
    void (*put_blocking)(void *ctx, uint8_t byte);

    /* Yuqoridagi uch funksiyaga uzatiladi (odatda &huart1) */
    void *ctx;
} LogSink;

/*
 * Loggerni ishga tushiradi. Ikki bosqichda chaqirilishi mumkin:
 *
 *   1) Log_Init(NULL) - eng boshida, BIRINCHI printf dan OLDIN.
 *      stdout ni statik buferli satrli rejimga o'tkazadi (newlib malloc
 *      qilmasin uchun). Kanal hali yo'q: printf lar halqa buferda
 *      to'planib turadi.
 *
 *   2) Log_Init(sink) - periferiya MX_..._Init() lardan keyin tayyor
 *      bo'lgach. Kanal ulanadi va 1-bosqichdan beri to'plangan hamma
 *      narsa darhol chiqib ketadi.
 *
 * Bir bosqichda ham ishlatsa bo'ladi - u holda undan oldingi printf lar
 * yo'qoladi. sink NULL yoki sink->start NULL bo'lsa kanal ulanmaydi.
 */
void Log_Init(const LogSink *sink);

/*
 * Halqa buferga nusxa oladi va darhol qaytadi - hech qachon bloklamaydi.
 * ISR ichidan ham xavfsiz.
 * Xabar sig'masa BUTUNLAY tashlanadi (yarim satr chiqmaydi) va hisobga
 * olinadi; joy bo'shagach bo'shliq o'rniga bir marta belgi qo'yiladi.
 */
void Log_Write(const char *data, uint16_t len);

/* Sink: fon uzatmasi muvaffaqiyatli tugadi */
void Log_TxDone(void);

/* Sink: fon uzatmasi xato bilan tugadi (bo'lak tashlanadi, log to'xtamaydi) */
void Log_TxFailed(void);

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
 * Buferdagi qoldiqni va msg ni sink ning put_blocking() i orqali
 * DMA/uzilishlarsiz chiqaradi (kanal berilgan bo'lsa), keyin LED larni
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
