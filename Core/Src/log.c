/*
 * log.c - USART1 uchun bloklamaydigan log chiqishi
 *
 *   printf -> _write -> halqa bufer -> DMA2_Stream7 -> USART1_TX (PA9)
 *
 * USART1 ni bu modul O'ZI to'liq boshqaradi:
 *   CubeMX da USART1 periferiyasi sozlanmagan (huart1 ham,
 *   MX_USART1_UART_Init() ham generatsiya qilinmaydi, HAL_UART_MODULE_ENABLED
 *   ham o'chirilgan). Log_Init() USART1 ni registr darajasida ko'taradi va
 *   uzatishni HAL DMA bilan to'g'ridan-to'g'ri &USART1->DR ga olib boradi.
 *   Shu sababli .ioc qayta generatsiya qilinganda log yo'li buzilmaydi.
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

/* USART1: PA9 = TX (AF7), 115200 8N1, faqat uzatish */
#define LOG_BAUDRATE 115200u
#define LOG_TX_PIN   GPIO_PIN_9
#define LOG_TX_PORT  GPIOA

#define BUF_MASK (LOG_TX_BUF_SIZE - 1)
#if (LOG_TX_BUF_SIZE & BUF_MASK) != 0
#error "LOG_TX_BUF_SIZE 2 ning darajasi bo'lishi kerak"
#endif

/*
 * Cheksiz aylanishga qarshi chegaralar.
 *
 * Log_Flush() ning timeout i HAL_GetTick() ga, u esa TIM1 uzilishiga
 * (TICK_INT_PRIORITY = 15, eng past) bog'liq. Uzilishlar o'chirilgan
 * holatda yoki TIM1 ni uza olmaydigan ISR ichidan (DMA2_Stream7 = 13,
 * OTG_HS = 0) chaqirilsa tick umuman o'smaydi. Bitta aylanish ~100 takt,
 * ya'ni 100k aylanish 168 MHz da ~6 ms, HSI 16 MHz da ~60 ms - ikkalasi
 * ham 1 ms lik tick davridan ancha uzun, shuncha vaqtda tick qimirlamasa
 * u umuman o'smaydi degani.
 */
#define LOG_FLUSH_STUCK_SPINS 100000u

/* panic_putc(): SystemCoreClock 0 yoki buzilgan bo'lsa ham TXE ni shuncha
   aylanish kutamiz */
#define PANIC_TX_GUARD_MIN    1000u

/* panic_puts(): NUL yo'qolgan bo'lsa ham shuncha belgidan keyin to'xtaymiz.
   Eng uzun haqiqiy xabar ~15 belgi. */
#define PANIC_PUTS_MAX        128u

static DMA_HandleTypeDef hdma_usart1_tx;

static uint8_t           s_buf[LOG_TX_BUF_SIZE];
static volatile uint16_t s_head;     /* keyingi yoziladigan joy */
static volatile uint16_t s_tail;     /* DMA o'qiyotgan joy */
static volatile uint16_t s_chunk;    /* joriy DMA uzatmasining uzunligi */
static volatile uint8_t  s_busy;     /* DMA hozir band */
static volatile uint8_t  s_ready;    /* Log_Init() muvaffaqiyatli bajarildi */
static volatile uint8_t  s_overflow; /* oxirgi belgilanishdan beri xabar tashlandi */
static volatile uint32_t s_dropped;
static volatile uint8_t  s_enabled = 1u; /* debug chiqishi (config dan) */

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
 * o'rtasida yuqori ustuvorlikdagi ISR kirib, ikkinchi
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
     qolgani keyingi DMA TC uzilishida yuboriladi */
  len = (head > tail) ? (uint16_t)(head - tail)
                      : (uint16_t)(LOG_TX_BUF_SIZE - tail);

  s_chunk = len;
  s_busy  = 1;

  if (HAL_DMA_Start_IT(&hdma_usart1_tx, (uint32_t)&s_buf[tail],
                       (uint32_t)&USART1->DR, len) != HAL_OK)
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
 * ~0.27us) sarflardi - 1KB lik yozuv uzilishlarni ~275us o'chirib turardi.
 * newlib-nano memcpy ham bayt sikli, lekin atigi 4 instruksiya/bayt
 * (~8 sikl) - ya'ni ~5.5 barobar tez. Eng yomon holat (2KB to'ldirish)
 * ~550us dan ~100us ga tushadi, shuncha vaqtga uzilishlarni kechiktirish
 * ETH/USB tayminggi uchun sezilmas.
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
  uint32_t    need;
  const char *p;
  uint16_t    remaining;

  if ((data == NULL) || (len == 0u))
  {
    return;
  }

  primask = __get_PRIMASK();
  __disable_irq();

  /* '\n' -> "\r\n" kengaytmasi uchun qancha qo'shimcha joy kerakligini
     sanaymiz.
     Sanash ham, quyidagi yozish ham BITTA kritik seksiya ichida bo'lishi
     SHART: ikkalasi ham 'data' ni o'qiydi. Sanash tashqarida qolsa,
     oralarida ISR 'data' ni o'zgartirib yuborishi mumkin edi (ISR ichidan
     printf qilinsa newlib ning bitta stdout buferi baham ko'riladi) -
     o'shanda ajratilgan joy haqiqatda yoziladigan baytdan kam bo'lib,
     ring_put() halqa buferda hali yuborilmagan ma'lumot ustiga yozardi.
     memchr ishlatiladi - -O0 da qo'lda yozilgan bayt siklidan ancha tez,
     256 baytlik satr uchun bir necha mikrosekund. */
  p         = data;
  remaining = len;
  while (remaining > 0u)
  {
    const char *nlp = (const char *)memchr(p, '\n', remaining);

    if (nlp == NULL)
    {
      break;
    }

    /* nlp diapazon ichida, ya'ni har aylanishda kamida 1 bayt iste'mol
       qilinadi - sikl albatta tugaydi va remaining underflow qilmaydi */
    nl++;
    remaining = (uint16_t)(remaining - (uint16_t)(nlp - p) - 1u);
    p         = nlp + 1;
  }

  need = (uint32_t)len + (uint32_t)nl;

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
  uint32_t spins = 0u;

  /* HAL_GetTick() TIM1 uzilishiga bog'liq, shuning uchun bu funksiya
     uzilishlar yoqilgan holatda chaqirilishi kerak. Noto'g'ri kontekstda
     chaqirilsa quyidagi shart hech qachon buzilmasdi - qarang
     LOG_FLUSH_STUCK_SPINS. */
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

    /* Tick qimirlamayapti: timeout hech qachon bitmaydi, kutishdan ma'no
       yo'q. Tick tirik bo'lsa bu shart ishlamaydi va timeout_ms odatdagidek
       oxirigacha kutiladi. */
    if ((++spins >= LOG_FLUSH_STUCK_SPINS) && (HAL_GetTick() == start))
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

/*
 * DMA va uzilishlarsiz bitta bayt. Cheklangan kutish - TXE hech qachon
 * kelmasa ham osilib qolmaymiz.
 *
 * Chegara SystemCoreClock ga bog'langan (panic_delay() bilan bir xil
 * uslub): 168 MHz da ham, PLL ko'tarilmay HSI 16 MHz da qolganda ham
 * kutish ~1 ms atrofida chiqadi. 115200 bod da bitta belgi ~87 us,
 * ya'ni zaxira ~10 barobar.
 *
 * Nega o'zgartirildi: avval bu 400000 ta qat'iy aylanish edi, ya'ni bayt
 * uchun ~20 ms. TX o'lik bo'lsa (TE o'chiq, PA9 tutashgan, klok buzilgan)
 * to'la buferni chiqarishga urinish o'nlab soniya davom etib, panik LED
 * lari shuncha vaqt ko'rinmasdi - osilib qolgandek tuyulardi. Endi eng
 * yomon holat ~2 soniya.
 */
static void panic_putc(char c)
{
  uint32_t guard = SystemCoreClock / 20000u;

  if (guard < PANIC_TX_GUARD_MIN)
  {
    guard = PANIC_TX_GUARD_MIN;       /* SystemCoreClock 0 yoki buzilgan */
  }

  while (((USART1->SR & USART_SR_TXE) == 0u) && (guard > 0u))
  {
    guard--;
  }
  USART1->DR = (uint16_t)((uint8_t)c);
}

/* NUL topilmasa ham to'xtaydi: bu funksiya fault ishlovchisidan
   chaqiriladi, u yerda xotira allaqachon buzilgan bo'lishi mumkin. */
static void panic_puts(const char *s)
{
  uint32_t guard = PANIC_PUTS_MAX;

  if (s == NULL)
  {
    return;
  }

  while ((*s != '\0') && (guard-- > 0u))
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
    /* Indekslarni mahalliy nusxaga olib, DARHOL maskalaymiz.
       Bu funksiya HardFault/MemManage/BusFault/UsageFault dan chaqiriladi,
       ya'ni sabab ko'pincha stack overflow yoki adashgan ko'rsatkich -
       o'sha paytda .bss dagi s_head/s_tail 0..BUF_MASK dan tashqarida
       bo'lishi mumkin. Maskasiz s_head = 3000 bo'lsa, s_tail faqat
       0..2047 qiymatlarni olgani uchun unga HECH QACHON yetmasdi: fault
       ishlovchisi shu yerda abadiy aylanib qolib, panik LED lari umuman
       yonmasdi (na UART, na LED - qurilma butunlay o'lik ko'rinardi),
       s_buf[s_tail] esa massivdan tashqariga chiqardi.
       Maskadan keyin tail 2048 qadamda albatta head ga teng bo'ladi;
       guard shu chegara sikl boshida ko'rinib tursin uchun. */
    uint16_t tail  = (uint16_t)(s_tail & BUF_MASK);
    uint16_t head  = (uint16_t)(s_head & BUF_MASK);
    uint16_t guard = LOG_TX_BUF_SIZE;

    DMA2_Stream7->CR &= ~DMA_SxCR_EN;   /* yarim qolgan uzatmani to'xtatamiz */

    while ((tail != head) && (guard-- > 0u))  /* buferdagi qoldiqni chiqaramiz */
    {
      panic_putc((char)s_buf[tail]);
      tail = (uint16_t)((tail + 1u) & BUF_MASK);
    }

    s_tail = tail;

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

/**
 * USART1 ni registr darajasida ko'taradi: PA9 -> AF7, 115200 8N1, TE + DMAT.
 * RX kerak emas (log faqat chiqish uchun), shuning uchun RE yoqilmaydi.
 */
static void usart1_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  uint32_t         pclk, div, mant, frac;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_USART1_CLK_ENABLE();

  gpio.Pin       = LOG_TX_PIN;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(LOG_TX_PORT, &gpio);

  USART1->CR1 = 0u;                   /* sozlashdan oldin o'chiramiz */
  USART1->CR2 = 0u;                   /* 1 stop bit */
  USART1->CR3 = USART_CR3_DMAT;       /* TX so'rovlari DMA ga boradi */

  /* BRR, OVER8=0 (16x sampling). HAL ning UART_BRR_SAMPLING16 mantiqi:
     kasr qism 16 ga yumaloqlansa, o'sha bir birlik mantissaga qo'shiladi.
     HAL_UART_MODULE_ENABLED o'chirilgani uchun makros mavjud emas - qo'lda. */
  pclk = HAL_RCC_GetPCLK2Freq();
  div  = (pclk * 25u) / (4u * LOG_BAUDRATE);
  mant = div / 100u;
  frac = (((div - (mant * 100u)) * 16u) + 50u) / 100u;
  USART1->BRR = (uint16_t)((mant << 4) + (frac & 0xF0u) + (frac & 0x0Fu));

  USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

/**
 * DMA bo'lagi tugadi: tail ni surib, navbatdagisini boshlaymiz.
 *
 * DMA TC oxirgi bayt DR ga yozilganda keladi - bayt hali shift registrda
 * bo'lishi mumkin, lekin buferdagi joy allaqachon bo'shagan, shuning uchun
 * keyingi uzatmani darhol boshlash xavfsiz (DMA baribir TXE ni kutadi).
 */
static void log_dma_cplt(DMA_HandleTypeDef *hdma)
{
  uint32_t primask = __get_PRIMASK();

  (void)hdma;

  __disable_irq();
  s_tail  = (uint16_t)((s_tail + s_chunk) & BUF_MASK);
  s_chunk = 0;
  s_busy  = 0;
  __set_PRIMASK(primask);

  log_kick();                     /* wrap bo'lgan qoldiq / yangi ma'lumot */
}

/**
 * DMA xatosi: joriy bo'lakni tashlab, navbatdagisiga o'tamiz - aks holda
 * s_busy abadiy 1 bo'lib qolib, log butunlay to'xtardi.
 */
static void log_dma_error(DMA_HandleTypeDef *hdma)
{
  uint32_t primask = __get_PRIMASK();

  (void)hdma;

  __disable_irq();
  s_dropped += s_chunk;
  s_tail  = (uint16_t)((s_tail + s_chunk) & BUF_MASK);
  s_chunk = 0;
  s_busy  = 0;
  __set_PRIMASK(primask);

  log_kick();
}

void Log_Init(void)
{
  /* Satrli buferlash: printf ichida malloc chaqirilmaydi va har bir log
     satri _write ga bitta chaqiruv bo'lib tushadi (belgi-belgi emas).
     Bufer 256 bayt - undan uzun satrni newlib bo'lib yuboradi, u holda
     "butun xabarni tashlash" kafolati bo'lak darajasida ishlaydi. */
  setvbuf(stdout, s_stdout_buf, _IOLBF, sizeof(s_stdout_buf));

  usart1_init();

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
    return;                       /* s_ready = 0 -> _write jimgina tashlaydi */
  }

  /* HAL_DMA_Start_IT bulardan TC va TE/DME uzilishlarini yoqadi;
     XferHalfCpltCallback NULL qolgani uchun HT uzilishi yoqilmaydi */
  hdma_usart1_tx.XferCpltCallback  = log_dma_cplt;
  hdma_usart1_tx.XferErrorCallback = log_dma_error;

  /* Log eng past ustuvorlikda - hech qachon boshqa periferiyalarning
     taymingini kechiktirmasin */
  HAL_NVIC_SetPriority(DMA2_Stream7_IRQn, 13, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream7_IRQn);

  s_ready = 1;
}

/* ================= Uzilish ishlovchisi =================
 * startup_stm32f407vetx.s da bu weak, shuning uchun bu yerda aniqlash mumkin.
 * Shu bilan CubeMX qayta generatsiyasi stm32f4xx_it.c ni yangilaganda ham log
 * yo'li buzilmaydi.
 * DIQQAT: agar keyinchalik CubeMX da DMA2_Stream7 ishlatilsa, stm32f4xx_it.c
 * ga DMA2_Stream7_IRQHandler qo'shiladi va dublikat simvol xatosi beradi -
 * u holda quyidagini o'chirish kerak.
 */

void DMA2_Stream7_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart1_tx);
}

void Log_SetEnabled(bool enabled)
{
  /* Buferda turgan xabarlar baribir chiqib bo'lsin - o'chirish paytida
     yarim yuborilgan satr terminalda osilib qolmaydi */
  s_enabled = enabled ? 1u : 0u;
}

/* ================= newlib retarget ================= */

int _write(int file, char *ptr, int len)
{
  int remaining = len;

  (void)file;

  /* len manfiy bo'lsa (uint16_t) ga o'girish uni 65535 ga yaqin ulkan
     songa aylantirar va Log_Write() bufer tashqarisini o'qib fault ga
     olib borardi */
  if (len <= 0)
  {
    return len;
  }

  if (!s_enabled)
  {
    return len;                     /* debug o'chirilgan - jimgina tashlanadi */
  }

  if (!s_ready)
  {
    return len;                     /* Log_Init() hali chaqirilmagan */
  }

  /* newlib odatda satrli buferdan (256 bayt) chaqiradi, lekin katta
     bloklarni buferni chetlab o'tib berishi ham mumkin. To'g'ridan-to'g'ri
     (uint16_t)len qilish 65536 baytni 0 ga aylantirib xabarni jimgina
     yo'qotardi, undan kattasini esa kesib yarim satr chiqarardi - bu
     modulning "yarim satr chiqmaydi" kafolatiga zid. Shuning uchun
     bo'lib beramiz; bufer sig'imidan katta bo'lak Log_Write() ning
     o'z siyosati bo'yicha butunlay tashlanadi va hisobga olinadi. */
  while (remaining > 0)
  {
    int chunk = (remaining > 65535) ? 65535 : remaining;

    Log_Write(ptr, (uint16_t)chunk);
    ptr       += chunk;
    remaining -= chunk;
  }

  return len;
}
