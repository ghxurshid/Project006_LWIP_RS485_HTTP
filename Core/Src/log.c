/*
 * log.c - USART1 uchun bloklamaydigan log chiqishi
 *
 *   printf -> _write -> halqa bufer -> DMA2_Stream7 -> USART1_TX (PA9)
 *
 * Nega DMA:
 *   Eski _write har bir baytni HAL_UART_Transmit(..., HAL_MAX_DELAY) bilan
 *   yuborardi. 115200 bod da bu bayt uchun ~87us bloklash, ya'ni 60 belgilik
 *   bitta log satri main loop ni ~5ms to'xtatib turadi. Shu vaqt ichida
 *   MX_LWIP_Process() chaqirilmaydi va kelayotgan ETH paketlari yo'qoladi.
 *   Bundan tashqari u qayta kirishga chidamli emas edi: ISR ichidan printf
 *   qilinsa HAL_BUSY qaytib, bayt jimgina yo'qolardi.
 *   Bu yerda printf faqat buferga nusxa oladi va darhol qaytadi; baytlarni
 *   fonda DMA uzatadi.
 *
 * To'lib ketish siyosati:
 *   DMA 115200 bod da atigi ~11.5 KB/s oqiza oladi, printf esa undan ancha
 *   tez yoza oladi. Shuning uchun bufer to'lishi normal holat deb qaraladi:
 *   sig'magan xabar BUTUNLAY tashlanadi, yarim yozilmaydi. Shunda terminalga
 *   chiqqan har bir satr to'liq bo'ladi. Joy bo'shagach bo'shliq o'rniga bir
 *   marta ogohlantirish satri qo'yiladi, umumiy hisob Log_DroppedBytes() da.
 *
 * Eslatma: DMA2 CCMRAM (0x1000_0000) ga murojaat qila olmaydi, shuning uchun
 * s_buf oddiy .bss (SRAM) da turishi shart - unga .ccmram atributi qo'yilmasin.
 *
 * Created on: 2025
 * Author: Xurshid Xujamatov
 */

#include "log.h"
#include "main.h"   /* LED*_Pin */
#include <string.h>

extern UART_HandleTypeDef huart1;

#define BUF_MASK (LOG_TX_BUF_SIZE - 1)
#if (LOG_TX_BUF_SIZE & BUF_MASK) != 0
#error "LOG_TX_BUF_SIZE 2 ning darajasi bo'lishi kerak"
#endif

static DMA_HandleTypeDef hdma_usart1_tx;

static uint8_t           s_buf[LOG_TX_BUF_SIZE];
static volatile uint16_t s_head;     /* keyingi yoziladigan joy */
static volatile uint16_t s_tail;     /* DMA o'qiyotgan joy */
static volatile uint16_t s_chunk;    /* joriy DMA uzatmasining uzunligi */
static volatile uint8_t  s_busy;     /* DMA hozir band */
static volatile uint8_t  s_ready;    /* Log_Init() muvaffaqiyatli bajarildi */
static volatile uint8_t  s_overflow; /* oxirgi belgilanishdan beri xabar tashlandi */
static volatile uint32_t s_dropped;

/* Tashlangan joyni ko'rsatuvchi belgi. printf orqali emas, to'g'ridan-to'g'ri
   buferga yoziladi - aks holda Log_Write o'zini rekursiv chaqirgan bo'lardi. */
static const char s_ovf_msg[] = "\r\n[LOG] <<< bufer to'ldi, xabarlar tashlandi >>>\r\n";
#define OVF_MSG_LEN (sizeof(s_ovf_msg) - 1u)

/* stdout buferi statik - newlib uni malloc qilib olmasligi uchun */
static char s_stdout_buf[256];

/**
 * Navbatdagi bo'lakni DMA ga beradi.
 *
 * O'zi kritik seksiya bilan himoyalangan: uni ham main loop, ham TxCplt
 * uzilishi chaqiradi. Himoyasiz bo'lsa, s_busy tekshiruvi bilan s_busy=1
 * o'rtasida yuqori ustuvorlikdagi ISR (masalan Wiegand) kirib, ikkinchi
 * DMA uzatmasini boshlab yuborishi va s_chunk ni buzishi mumkin edi.
 * Ichma-ich chaqirilsa ham xavfsiz - PRIMASK saqlanib tiklanadi.
 */
static void log_kick(void)
{
  uint32_t primask = __get_PRIMASK();
  uint16_t head, tail, len;

  __disable_irq();

  if (s_busy || !s_ready)
  {
    goto out;
  }

  head = s_head;
  tail = s_tail;
  if (head == tail)
  {
    goto out;                       /* bufer bo'sh */
  }

  /* DMA faqat uzluksiz blokni yubora oladi - buferning oxirigacha kesamiz,
     qolgani keyingi TxCplt da yuboriladi */
  len = (head > tail) ? (uint16_t)(head - tail)
                      : (uint16_t)(LOG_TX_BUF_SIZE - tail);

  s_chunk = len;
  s_busy  = 1;

  if (HAL_UART_Transmit_DMA(&huart1, &s_buf[tail], len) != HAL_OK)
  {
    s_chunk = 0;
    s_busy  = 0;
  }

out:
  __set_PRIMASK(primask);
}

/** Bo'sh joy. Uzilishlar o'chirilgan holatda chaqirilishi shart. */
static uint16_t ring_free(void)
{
  return (uint16_t)((s_tail - s_head - 1u) & BUF_MASK);
}

/**
 * n baytni buferga ko'chiradi (kerak bo'lsa ikki bo'lakka bo'lib).
 * Chaqiruvchi n <= ring_free() ekanini kafolatlaydi.
 * Uzilishlar o'chirilgan holatda chaqirilishi shart.
 *
 * memcpy ishlatiladi: qo'lda yozilgan bayt-bayt sikl volatile s_head/s_tail
 * ni har safar qayta yuklab, -O0 da bayt uchun ~38 instruksiya (~45 sikl,
 * ~0.27us) sarflardi - 1KB lik yozuv uzilishlarni ~275us o'chirib turib,
 * Wiegand impulsini o'tkazib yuborishi mumkin edi.
 * newlib-nano memcpy ham bayt sikli, lekin atigi 4 instruksiya/bayt
 * (~8 sikl) - ya'ni ~5.5 barobar tez. Eng yomon holat (2KB to'ldirish)
 * ~550us dan ~100us ga tushadi, bu Wiegand ning 200us lik minimal bit
 * oralig'idan past, demak bit yo'qolmaydi.
 */
static void ring_put(const char *src, uint16_t n)
{
  uint16_t head  = s_head;
  uint16_t first = (uint16_t)(LOG_TX_BUF_SIZE - head);

  if (n <= first)
  {
    memcpy(&s_buf[head], src, n);
  }
  else
  {
    memcpy(&s_buf[head], src, first);
    memcpy(&s_buf[0], src + first, (uint16_t)(n - first));
  }

  s_head = (uint16_t)((head + n) & BUF_MASK);
}

void Log_Write(const char *data, uint16_t len)
{
  uint32_t    primask;
  uint16_t    nl = 0;
  uint16_t    i;
  uint32_t    need;
  const char *p;
  uint16_t    remaining;

  if (len == 0u)
  {
    return;
  }

  /* '\n' -> "\r\n" kengaytmasi uchun qancha qo'shimcha joy kerakligini
     oldindan sanaymiz. Bu kritik seksiyadan TASHQARIDA bajariladi. */
  for (i = 0; i < len; i++)
  {
    if (data[i] == '\n')
    {
      nl++;
    }
  }
  need = (uint32_t)len + (uint32_t)nl;

  primask = __get_PRIMASK();
  __disable_irq();

  if (need > (uint32_t)ring_free())
  {
    /* Butun xabarni tashlaymiz - yarim yozilgan satr chiqargandan ko'ra
       umuman chiqarmagan yaxshi. Xabar buferdan ham katta bo'lsa
       (head==tail bo'sh degani uchun sig'im LOG_TX_BUF_SIZE-1) u hech
       qachon sig'maydi va har safar tashlanadi. */
    s_dropped  += len;
    s_overflow  = 1;
    __set_PRIMASK(primask);
    log_kick();                     /* DMA to'xtab qolgan bo'lsa turtamiz */
    return;
  }

  /* Bo'shliq bo'lgan joyga bir marta belgi qo'yamiz - ikkalasiga joy
     yetsagina, aks holda belgi keyingi safarga qoladi */
  if (s_overflow && ((need + OVF_MSG_LEN) <= (uint32_t)ring_free()))
  {
    ring_put(s_ovf_msg, (uint16_t)OVF_MSG_LEN);
    s_overflow = 0;
  }

  /* Satr bo'laklarini memcpy bilan ko'chiramiz, '\n' larni "\r\n" ga
     aylantirib */
  p         = data;
  remaining = len;
  while (remaining > 0u)
  {
    const char *nlp = (const char *)memchr(p, '\n', remaining);
    uint16_t    run = (nlp != NULL) ? (uint16_t)(nlp - p) : remaining;

    if (run > 0u)
    {
      ring_put(p, run);
    }

    if (nlp != NULL)
    {
      ring_put("\r\n", 2u);
      run++;                        /* '\n' ni ham iste'mol qildik */
    }

    p         += run;
    remaining -= run;
  }

  __set_PRIMASK(primask);

  log_kick();
}

void Log_Flush(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();

  /* HAL_GetTick() TIM1 uzilishiga bog'liq, shuning uchun bu funksiya
     uzilishlar yoqilgan holatda chaqirilishi kerak. */
  while ((HAL_GetTick() - start) < timeout_ms)
  {
    uint32_t primask = __get_PRIMASK();
    uint8_t  empty;

    __disable_irq();
    empty = (uint8_t)((s_head == s_tail) && (s_busy == 0u));
    __set_PRIMASK(primask);

    log_kick();                   /* to'xtab qolgan bo'lsa qayta turtamiz */

    if (empty)
    {
      return;
    }
  }
}

uint32_t Log_DroppedBytes(void)
{
  return s_dropped;
}

/* Panik holatida HAL_Delay/HAL_GetTick ga ishonib bo'lmaydi: uzilishlar
   o'chirilgan yoki klok umuman noto'g'ri bo'lishi mumkin */
static void panic_delay(uint32_t loops)
{
  volatile uint32_t i;
  for (i = 0; i < loops; i++)
  {
    __NOP();
  }
}

/* DMA va uzilishlarsiz bitta bayt. Cheklangan kutish - TXE hech qachon
   kelmasa ham osilib qolmaymiz. */
static void panic_putc(char c)
{
  uint32_t guard = 400000u;

  while (((USART1->SR & USART_SR_TXE) == 0u) && (guard > 0u))
  {
    guard--;
  }
  USART1->DR = (uint16_t)((uint8_t)c);
}

static void panic_puts(const char *s)
{
  while (*s != '\0')
  {
    panic_putc(*s++);
  }
}

/* LED2 ni chiqish qilib sozlaydi (bir marta yetadi, lekin qayta chaqirish
   ham zararsiz) */
static void boot_led_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOE_CLK_ENABLE();
  gpio.Pin   = LED2_Pin;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED2_GPIO_Port, &gpio);
}

void Boot_Mark(uint8_t blinks)
{
  /* SystemCoreClock ga moslangan: klok HSI 16MHz bo'lsa ham, PLL 168MHz
     bo'lsa ham miltillash bir xil tezlikda ko'rinadi */
  const uint32_t on_off = SystemCoreClock / 60u;
  uint8_t        n;

  boot_led_init();

  for (n = 0; n < blinks; n++)
  {
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET);
    panic_delay(on_off);
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
    panic_delay(on_off);
  }

  panic_delay(on_off * 6u);           /* bosqichlar orasidagi pauza */
}

void Log_Panic(const char *msg, uint8_t blinks)
{
  GPIO_InitTypeDef gpio = {0};

  __disable_irq();

  /* USART1 kloki yoqilgan bo'lsagina chiqarishga urinamiz - aks holda
     klok berilmagan periferiyaga murojaat qilgan bo'lardik */
  if ((RCC->APB2ENR & RCC_APB2ENR_USART1EN) != 0u)
  {
    DMA2_Stream7->CR &= ~DMA_SxCR_EN;   /* yarim qolgan uzatmani to'xtatamiz */

    while (s_tail != s_head)            /* buferdagi qoldiqni chiqaramiz */
    {
      panic_putc((char)s_buf[s_tail]);
      s_tail = (uint16_t)((s_tail + 1u) & BUF_MASK);
    }

    if (msg != NULL)
    {
      panic_puts("\r\n[PANIC] ");
      panic_puts(msg);
      panic_puts("\r\n");
    }
  }

  /* LED larni o'zimiz sozlaymiz - MX_GPIO_Init() ishlamagan bo'lishi mumkin */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  gpio.Pin   = LED1_Pin | LED2_Pin | LED3_Pin;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &gpio);

  for (;;)
  {
    uint8_t n;

    for (n = 0; n < blinks; n++)
    {
      HAL_GPIO_WritePin(GPIOE, LED1_Pin | LED3_Pin, GPIO_PIN_RESET);
      panic_delay(2000000u);
      HAL_GPIO_WritePin(GPIOE, LED1_Pin | LED3_Pin, GPIO_PIN_SET);
      panic_delay(2000000u);
    }
    panic_delay(12000000u);            /* naqshlar orasidagi uzun pauza */
  }
}

void Log_Init(void)
{
  /* Satrli buferlash: printf ichida malloc chaqirilmaydi va har bir log
     satri _write ga bitta chaqiruv bo'lib tushadi (belgi-belgi emas).
     Bufer 256 bayt - undan uzun satrni newlib bo'lib yuboradi, u holda
     "butun xabarni tashlash" kafolati bo'lak darajasida ishlaydi. */
  setvbuf(stdout, s_stdout_buf, _IOLBF, sizeof(s_stdout_buf));

  __HAL_RCC_DMA2_CLK_ENABLE();

  /* USART1_TX = DMA2, Stream 7, Channel 4 (RM0090, Table 43) */
  hdma_usart1_tx.Instance                 = DMA2_Stream7;
  hdma_usart1_tx.Init.Channel             = DMA_CHANNEL_4;
  hdma_usart1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_usart1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_usart1_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_usart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_usart1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_usart1_tx.Init.Mode                = DMA_NORMAL;
  hdma_usart1_tx.Init.Priority            = DMA_PRIORITY_LOW;
  hdma_usart1_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

  if (HAL_DMA_Init(&hdma_usart1_tx) != HAL_OK)
  {
    return;                       /* s_ready = 0 -> _write zaxira yo'lga o'tadi */
  }

  __HAL_LINKDMA(&huart1, hdmatx, hdma_usart1_tx);

  /* Log Wiegand (EXTI, 0), RS485 (USART2/DMA1, 0) dan past ustuvorlikda -
     hech qachon ularning tayminggini kechiktirmasin */
  HAL_NVIC_SetPriority(DMA2_Stream7_IRQn, 13, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream7_IRQn);

  /* DMA tugagach HAL TC uzilishi orqali TxCpltCallback ni chaqiradi,
     shuning uchun USART1 global uzilishi ham kerak */
  HAL_NVIC_SetPriority(USART1_IRQn, 13, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);

  s_ready = 1;
}

/* ================= Uzilish ishlovchilari =================
 * startup_stm32f407vetx.s da bular weak, shuning uchun bu yerda aniqlash
 * mumkin. Shu bilan CubeMX qayta generatsiyasi stm32f4xx_it.c ni yangilaganda
 * ham log yo'li buzilmaydi.
 * DIQQAT: agar keyinchalik CubeMX da USART1 global interrupt yoqilsa,
 * stm32f4xx_it.c ga USART1_IRQHandler qo'shiladi va dublikat simvol xatosi
 * beradi - u holda quyidagi ikkitasini o'chirish kerak.
 */

void DMA2_Stream7_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart1_tx);
}

void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART1)
  {
    return;
  }

  {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_tail  = (uint16_t)((s_tail + s_chunk) & BUF_MASK);
    s_chunk = 0;
    s_busy  = 0;
    __set_PRIMASK(primask);
  }

  log_kick();                     /* wrap bo'lgan qoldiq / yangi ma'lumot */
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART1)
  {
    return;                       /* USART2 xatolari bu yerda ishlanmaydi */
  }

  /* DMA/UART xatosi: joriy bo'lakni tashlab, navbatdagisiga o'tamiz -
     aks holda s_busy abadiy 1 bo'lib qolib, log butunlay to'xtardi */
  {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_dropped += s_chunk;
    s_tail  = (uint16_t)((s_tail + s_chunk) & BUF_MASK);
    s_chunk = 0;
    s_busy  = 0;
    __set_PRIMASK(primask);
  }

  log_kick();
}

/* ================= newlib retarget ================= */

int _write(int file, char *ptr, int len)
{
  (void)file;

  if (s_ready)
  {
    Log_Write(ptr, (uint16_t)len);
  }
  else if (huart1.gState == HAL_UART_STATE_READY)
  {
    /* Log_Init() gacha bo'lgan chiqish uchun bloklovchi zaxira yo'l */
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100);
  }
  /* huart1 hali init qilinmagan bo'lsa - jimgina tashlanadi */

  return len;
}
