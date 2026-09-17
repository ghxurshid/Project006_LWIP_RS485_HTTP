/*
 * log.c - bloklamaydigan, halqa buferli log chiqishi
 *
 *   printf -> _write -> halqa bufer -> LogSink -> periferiya
 *
 * Bu modul PERIFERIYANI BILMAYDI (qarang log.h dagi LogSink). Periferiya
 * CubeMX da sozlanadi, tayyor handle Log_Init() ga tashqaridan beriladi.
 * Shu sababli:
 *   - .ioc qayta generatsiya qilinganda bu fayl o'zgarmaydi va CubeMX
 *     sozlagan pin/klok/NVIC ustiga hech narsa yozilmaydi;
 *   - printf ni boshqa kanalga burish uchun faqat yangi sink yoziladi.
 *
 * Nega halqa bufer va fon uzatmasi:
 *   Bloklab yuborish 115200 bod da bayt uchun ~87us, ya'ni 60 belgilik
 *   bitta log satri main loop ni ~5ms to'xtatib turadi. Shu vaqt ichida
 *   MX_LWIP_Process() chaqirilmaydi va kelayotgan ETH paketlari yo'qoladi.
 *   Bundan tashqari bloklab yuborish qayta kirishga chidamli emas: ISR
 *   ichidan printf qilinsa HAL_BUSY qaytib, bayt jimgina yo'qolardi.
 *   Bu yerda printf faqat buferga nusxa oladi va darhol qaytadi; baytlarni
 *   fonda sink uzatadi.
 *
 * To'lib ketish siyosati:
 *   115200 bod atigi ~11.5 KB/s oqiza oladi, printf esa undan ancha tez
 *   yoza oladi. Shuning uchun bufer to'lishi normal holat deb qaraladi:
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

#define BUF_MASK (LOG_TX_BUF_SIZE - 1)
#if (LOG_TX_BUF_SIZE & BUF_MASK) != 0
#error "LOG_TX_BUF_SIZE 2 ning darajasi bo'lishi kerak"
#endif

/*
 * Cheksiz aylanishga qarshi chegaralar.
 *
 * Log_Flush() ning timeout i HAL_GetTick() ga, u esa TIM1 uzilishiga
 * (TICK_INT_PRIORITY = 15, eng past) bog'liq. Uzilishlar o'chirilgan
 * holatda yoki TIM1 ni uza olmaydigan ISR ichidan chaqirilsa tick umuman
 * o'smaydi. Bitta aylanish ~100 takt, ya'ni 100k aylanish 168 MHz da
 * ~6 ms, HSI 16 MHz da ~60 ms - ikkalasi ham 1 ms lik tick davridan ancha
 * uzun, shuncha vaqtda tick qimirlamasa u umuman o'smaydi degani.
 */
#define LOG_FLUSH_STUCK_SPINS 100000u

/* panic_puts(): NUL yo'qolgan bo'lsa ham shuncha belgidan keyin to'xtaymiz.
   Eng uzun haqiqiy xabar ~15 belgi. */
#define PANIC_PUTS_MAX        128u

/* Tashqaridan berilgan chiqish kanali. NULL - kanal hali ulanmagan:
   printf lar buferda to'planadi, lekin uzatilmaydi. */
static const LogSink    *s_sink;

static uint8_t           s_buf[LOG_TX_BUF_SIZE];
static volatile uint16_t s_head;     /* keyingi yoziladigan joy */
static volatile uint16_t s_tail;     /* sink o'qiyotgan joy */
static volatile uint16_t s_chunk;    /* joriy uzatmaning uzunligi */
static volatile uint8_t  s_busy;     /* sink hozir band */
static volatile uint8_t  s_ready;    /* Log_Init() kamida bir marta chaqirildi */
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
 * Navbatdagi bo'lakni sink ga beradi.
 *
 * O'zi kritik seksiya bilan himoyalangan: uni ham main loop, ham uzatma
 * tugaganini bildiruvchi ISR (Log_TxDone) chaqiradi. Himoyasiz bo'lsa,
 * s_busy tekshiruvi bilan s_busy=1 o'rtasida yuqori ustuvorlikdagi ISR
 * kirib, ikkinchi uzatmani boshlab yuborishi va s_chunk ni buzishi mumkin
 * edi. Ichma-ich chaqirilsa ham xavfsiz - PRIMASK saqlanib tiklanadi.
 */
static void log_kick(void)
{
  uint32_t primask = __get_PRIMASK();
  uint16_t head, tail, len;

  __disable_irq();

  if (s_busy || !s_ready || (s_sink == NULL))
  {
    goto out;
  }

  head = s_head;
  tail = s_tail;
  if (head == tail)
  {
    goto out;                       /* bufer bo'sh */
  }

  /* Ko'pchilik kanal (DMA) faqat uzluksiz blokni yubora oladi - buferning
     oxirigacha kesamiz, qolgani keyingi Log_TxDone() da yuboriladi */
  len = (head > tail) ? (uint16_t)(head - tail)
                      : (uint16_t)(LOG_TX_BUF_SIZE - tail);

  s_chunk = len;
  s_busy  = 1;

  if (!s_sink->start(s_sink->ctx, &s_buf[tail], len))
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
    log_kick();                     /* sink to'xtab qolgan bo'lsa turtamiz */
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

/* ================= Sink dan keladigan xabarlar ================= */

void Log_TxDone(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  s_tail  = (uint16_t)((s_tail + s_chunk) & BUF_MASK);
  s_chunk = 0;
  s_busy  = 0;
  __set_PRIMASK(primask);

  log_kick();                     /* wrap bo'lgan qoldiq / yangi ma'lumot */
}

/*
 * Uzatma xatosi: joriy bo'lakni tashlab, navbatdagisiga o'tamiz - aks holda
 * s_busy abadiy 1 bo'lib qolib, log butunlay to'xtardi.
 */
void Log_TxFailed(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  s_dropped += s_chunk;
  s_tail  = (uint16_t)((s_tail + s_chunk) & BUF_MASK);
  s_chunk = 0;
  s_busy  = 0;
  __set_PRIMASK(primask);

  log_kick();
}

void Log_Flush(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  uint32_t spins = 0u;

  /* Kanal ulanmagan bo'lsa bufer hech qachon bo'shamaydi - butun timeout ni
     bekorga kutib turmaymiz */
  if (!s_ready || (s_sink == NULL))
  {
    return;
  }

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

/* ================= Panik yo'li ================= */

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

/* Bitta bayt sink orqali. Kanal put_blocking() bermasa jimgina tashlanadi -
   LED signali baribir ishlaydi. Cheklangan vaqtda qaytish mas'uliyati
   sink da (qarang log.h). */
static void panic_putc(char c)
{
  if ((s_sink != NULL) && (s_sink->put_blocking != NULL))
  {
    s_sink->put_blocking(s_sink->ctx, (uint8_t)c);
  }
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

  /* Kanal berilgan va u bloklab yozishni qo'llab-quvvatlasagina matn
     chiqarishga urinamiz */
  if ((s_sink != NULL) && (s_sink->put_blocking != NULL))
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

    if (s_sink->abort != NULL)
    {
      s_sink->abort(s_sink->ctx);     /* yarim qolgan fon uzatmasini to'xtatamiz */
    }

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

/* ================= Ishga tushirish ================= */

void Log_Init(const LogSink *sink)
{
  if (!s_ready)
  {
    /* Satrli buferlash: printf ichida malloc chaqirilmaydi va har bir log
       satri _write ga bitta chaqiruv bo'lib tushadi (belgi-belgi emas).
       Bufer 256 bayt - undan uzun satrni newlib bo'lib yuboradi, u holda
       "butun xabarni tashlash" kafolati bo'lak darajasida ishlaydi.
       BIRINCHI printf dan OLDIN bajarilishi shart, aks holda newlib
       stdout uchun buferni o'zi malloc qilib oladi. */
    setvbuf(stdout, s_stdout_buf, _IOLBF, sizeof(s_stdout_buf));
    s_ready = 1;
  }

  /* start() majburiy - usiz kanal bilan gaplashib bo'lmaydi. abort() va
     put_blocking() ixtiyoriy, ular faqat panik yo'lida ishlatiladi. */
  if ((sink != NULL) && (sink->start != NULL))
  {
    s_sink = sink;
  }

  /* Kanal endi ulangan bo'lsa, 1-bosqichdan beri to'plangani chiqib ketadi */
  log_kick();
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
