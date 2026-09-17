/*
 * log_sink_uart.h - CubeMX sozlagan UART ni log kanaliga aylantiradi
 *
 * Periferiyani BU MODUL SOZLAMAYDI. UART, uning pinlari, kloki, DMA si va
 * NVIC i CubeMX da (.ioc) sozlanadi; bu yerga faqat tayyor handle beriladi:
 *
 *     MX_USART1_UART_Init();                     <- CubeMX
 *     Log_Init(LogSinkUart_Create(&huart1));     <- shu modul
 *
 * CubeMX da kerak bo'ladigan minimum:
 *   - USART1 -> Asynchronous, 115200 8N1, Mode = TX Only (yoki TX/RX)
 *   - PA9 signali USART1_TX bo'lsin (GPIO_Output bo'lib qolmasin!)
 *   - DMA Settings -> Add -> USART1_TX, Normal, Memory increment
 *   - NVIC -> USART1 global interrupt YOQILGAN bo'lsin (DMA TC uzilishi
 *     HAL ning UART qatlamidan o'tadi)
 *
 * DMA sozlanmagan bo'lsa modul avtomatik HAL_UART_Transmit_IT() ga
 * tushadi - u holda ham NVIC da USART1 uzilishi yoqilgan bo'lishi shart.
 *
 * Bitta nusxa: ayni paytda faqat bitta UART log kanali bo'lishi mumkin.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#ifndef LOG_SINK_UART_H_
#define LOG_SINK_UART_H_

#include "log.h"
#include "stm32f4xx_hal.h"

#ifdef HAL_UART_MODULE_ENABLED

/*
 * huart ni log kanaliga o'raydi va Log_Init() ga beriladigan sink ni
 * qaytaradi. huart MX_..._UART_Init() dan KEYIN berilishi kerak.
 * huart yoki huart->Instance NULL bo'lsa NULL qaytadi - u holda
 * Log_Init() kanalsiz qoladi va printf jimgina tashlanadi.
 */
const LogSink *LogSinkUart_Create(UART_HandleTypeDef *huart);

/*
 * HAL uzatma callback lari.
 *
 * Bu modul HAL_UART_TxCpltCallback() va HAL_UART_ErrorCallback() ning
 * __weak nusxalarini o'zi aniqlaydi va ular shu ikkitasini chaqiradi.
 * Agar loyihada boshqa UART uchun O'Z callback ingiz bo'lsa, linker
 * konflikti chiqadi - o'shanda log_sink_uart.c dagi tayyor
 * HAL_UART_*Callback() larni o'chirib, o'zingiznikidan shu ikkitasini
 * chaqiring. Ular handle ni tekshiradi, boshqa UART larga tegmaydi.
 */
void LogSinkUart_OnTxCplt(UART_HandleTypeDef *huart);
void LogSinkUart_OnError(UART_HandleTypeDef *huart);

#endif /* HAL_UART_MODULE_ENABLED */

#endif /* LOG_SINK_UART_H_ */
