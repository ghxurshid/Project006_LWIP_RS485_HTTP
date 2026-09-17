/*
 * log_sink_uart.c - log.h dagi LogSink ning HAL UART uchun realizatsiyasi
 *
 * Bu yerda periferiya SOZLANMAYDI - hammasi CubeMX da. Modul faqat tayyor
 * UART_HandleTypeDef ustiga quriladi va uni logger ko'radigan uch amalga
 * (start / abort / put_blocking) moslaydi.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#include "log_sink_uart.h"

#ifdef HAL_UART_MODULE_ENABLED

/* Panik yo'lida TXE ni kutish chegarasi.
 * SystemCoreClock ga bog'langan: 168 MHz da ham, PLL ko'tarilmay HSI
 * 16 MHz da qolganda ham kutish ~1 ms atrofida chiqadi. 115200 bod da
 * bitta belgi ~87 us, ya'ni zaxira ~10 barobar. To'la buferni (2047 bayt)
 * chiqarishga urinish eng yomon holatda ~2 soniya - shundan keyin panik
 * LED lariga o'tiladi. */
#define PANIC_TX_GUARD_DIV  20000u
#define PANIC_TX_GUARD_MIN  1000u

static UART_HandleTypeDef *s_huart;
static LogSink             s_sink;

/* ================= LogSink amallari ================= */

/*
 * Logger buni uzilishlar o'chirilgan holatda chaqiradi, shuning uchun
 * faqat bloklamaydigan HAL chaqiruvlari ishlatiladi.
 *
 * DMA ulangan bo'lsa DMA bilan, aks holda uzilish bilan yuboramiz - shunda
 * CubeMX da UART ga DMA qo'shilmagan bo'lsa ham log ishlayveradi (faqat
 * sekinroq va CPU ni ko'proq band qiladi).
 */
static bool uart_start(void *ctx, const uint8_t *data, uint16_t len)
{
  UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;
  HAL_StatusTypeDef   st;

  /* HAL bu API larda const olmaydi; bufer uzatma davomida o'zgarmaydi */
  uint8_t *buf = (uint8_t *)(uintptr_t)data;

  if (h->hdmatx != NULL)
  {
    st = HAL_UART_Transmit_DMA(h, buf, len);
  }
  else
  {
    st = HAL_UART_Transmit_IT(h, buf, len);
  }

  /* HAL_BUSY bo'lsa false qaytaramiz - ma'lumot buferda qoladi va logger
     keyingi yozuvda qayta uradi */
  return (st == HAL_OK);
}

/*
 * Panik oldidan fon uzatmasini to'xtatish.
 *
 * HAL_UART_Abort() ATAYLAB ishlatilmadi: u ichida HAL_DMA_Abort() ni
 * chaqiradi, u esa HAL_GetTick() ga tayangan timeout bilan kutadi. Panik
 * paytida uzilishlar o'chirilgan, ya'ni tick o'smaydi va o'sha kutish
 * abadiy davom etardi.
 */
static void uart_abort(void *ctx)
{
  UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;

  if (h->hdmatx != NULL)
  {
    __HAL_DMA_DISABLE(h->hdmatx);
  }

  /* Stream o'chgach USART dan DMA so'rovi ketmasin */
  CLEAR_BIT(h->Instance->CR3, USART_CR3_DMAT);

  h->gState = HAL_UART_STATE_READY;
}

/*
 * DMA va uzilishlarsiz bitta bayt. Chiqish hech qachon tayyor bo'lmasa ham
 * cheklangan vaqtda qaytadi - qarang PANIC_TX_GUARD_DIV.
 */
static void uart_put_blocking(void *ctx, uint8_t byte)
{
  UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;
  uint32_t            guard;

  /* Periferiya yoqilmagan bo'lsa yozib o'tirmaymiz. F4 da kloki
     berilmagan periferiya registri 0 qaytaradi, shuning uchun bu tekshiruv
     "klok o'chiq" holatini ham tutadi. */
  if ((h->Instance->CR1 & USART_CR1_UE) == 0u)
  {
    return;
  }

  guard = SystemCoreClock / PANIC_TX_GUARD_DIV;
  if (guard < PANIC_TX_GUARD_MIN)
  {
    guard = PANIC_TX_GUARD_MIN;       /* SystemCoreClock 0 yoki buzilgan */
  }

  while (((h->Instance->SR & USART_SR_TXE) == 0u) && (guard > 0u))
  {
    guard--;
  }

  h->Instance->DR = (uint16_t)byte;
}

/* ================= Qurish ================= */

const LogSink *LogSinkUart_Create(UART_HandleTypeDef *huart)
{
  if ((huart == NULL) || (huart->Instance == NULL))
  {
    return NULL;                      /* Log_Init() kanalsiz qoladi */
  }

  s_huart = huart;

  s_sink.start        = uart_start;
  s_sink.abort        = uart_abort;
  s_sink.put_blocking = uart_put_blocking;
  s_sink.ctx          = huart;

  return &s_sink;
}

/* ================= HAL callback lari ================= */

void LogSinkUart_OnTxCplt(UART_HandleTypeDef *huart)
{
  if (huart == s_huart)
  {
    Log_TxDone();
  }
}

void LogSinkUart_OnError(UART_HandleTypeDef *huart)
{
  if (huart != s_huart)
  {
    return;
  }

  /*
   * RX tomonidagi xatolar (ORE/NE/FE/PE) uzatmaga tegishli emas. Ularga
   * javoban bo'lakni "yuborilgan" deb belgilasak, hali chiqmagan satrlar
   * yo'qolardi - shuning uchun faqat DMA/uzatma xatosini hisobga olamiz.
   */
  if ((huart->ErrorCode & HAL_UART_ERROR_DMA) != 0u)
  {
    Log_TxFailed();
  }
}

/*
 * HAL bularni __weak qilib e'lon qiladi, shuning uchun shu yerda aniqlash
 * mumkin. Loyihada boshqa UART uchun o'z callback ingiz paydo bo'lsa,
 * linker "multiple definition" beradi - o'shanda quyidagi ikkitasini
 * o'chirib, o'zingiznikidan LogSinkUart_OnTxCplt()/OnError() ni chaqiring.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  LogSinkUart_OnTxCplt(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  LogSinkUart_OnError(huart);
}

#endif /* HAL_UART_MODULE_ENABLED */
