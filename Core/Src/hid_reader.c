/*
 * hid_reader.c - USB HID klaviatura hisobotlaridan QR kodni yig'ish
 *
 * Hisobotlar usbh_hidkbd drayveridan qurilma formatidan qat'i nazar bir xil
 * ko'rinishda keladi (modifierlar + 6 ta tugma). Bu modul ularni matnga
 * aylantiradi va tayyor kodni queue ga qo'yadi.
 *
 * Nega har hisobotdagi "yangi bosilgan" tugmalar olinadi:
 *   Skaner ikki belgini orada qo'yib yubormasdan yuborsa ([1E] -> [1E 1F])
 *   faqat birinchi katakka qaraydigan dekoder ikkinchisini yo'qotardi, bitta
 *   tugma ikki hisobotda turib qolsa esa ikki marta yozardi. Shuning uchun
 *   oldingi hisobot bilan solishtiriladi va faqat avval yo'q bo'lgan tugmalar
 *   belgiga aylantiriladi.
 *
 * Nega main loop to'xtab qolishi hisobga olinadi:
 *   SendDataRawTCP() va LED animatsiyalari main loop ni yuzlab ms ushlab
 *   turadi. Bu vaqtda host hisobot so'ramaydi, belgilar skanerning o'zida
 *   navbatda turadi - ya'ni bu skanerlashdagi pauza EMAS. Hisobga olinmasa
 *   kod o'rtasidan bo'linib, ikkita noto'g'ri raqam queue ga tushardi.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#include "hid_reader.h"
#include "usbh_hidkbd.h"
#include "log.h"
#include <string.h>

/* Ikki HidReader_Process() chaqiruvi orasi bundan uzun bo'lsa main loop
   bloklangan deb hisoblanadi (normal aylanish mikrosekundlarda). */
#define LOOP_STALL_MS 50u

/* Keyboard usage page kodlari (HID Usage Tables, 0x07) */
#define USAGE_ERROR_ROLLOVER 0x01u
#define USAGE_ENTER          0x28u
#define USAGE_TAB            0x2Bu
#define USAGE_KEYPAD_ENTER   0x58u

#define MODS_SHIFT   (HIDKBD_MOD_LSHIFT | HIDKBD_MOD_RSHIFT)
#define MODS_COMMAND (HIDKBD_MOD_LCTRL | HIDKBD_MOD_RCTRL | HIDKBD_MOD_LALT | \
                      HIDKBD_MOD_RALT | HIDKBD_MOD_LGUI | HIDKBD_MOD_RGUI)

typedef enum {
    DEV_NONE,         /* hech narsa ulanmagan */
    DEV_ENUMERATING,  /* ulandi, enumeratsiya ketmoqda */
    DEV_READY,        /* klaviatura interfeysi ishlayapti */
    DEV_FAILED,       /* qo'llab-quvvatlanmadi yoki enumeratsiya xatosi */
} DevState;

extern USBH_HandleTypeDef hUsbHostHS;  /* usb_host.c */

static Queue    *s_queue;
static DevState  s_devState;

static char      s_line[HID_READER_LINE_MAX + 1u];
static uint8_t   s_len;
static bool      s_overflow;                    /* satr sig'madi - terminatorgacha tashlanadi */
static uint8_t   s_prevKeys[HIDKBD_MAX_KEYS];   /* oldingi hisobotdagi bosilgan tugmalar */
static uint32_t  s_lastKeyTick;
static uint32_t  s_lastProcessTick;

// ============== Satrni yig'ish ==============

static void ClearScan(void)
{
    s_len = 0u;
    s_overflow = false;
}

/* " 00123 " -> 123. Faqat raqamlar (atrofida bo'shliq bo'lishi mumkin);
   0 va HID_READER_MAX_VALUE dan kattasi rad etiladi. */
static bool ParseValue(const char *s, uint8_t len, uint64_t *out)
{
    uint8_t start = 0u;
    uint8_t end = len;

    while (start < end && s[start] == ' ')
        start++;
    while (end > start && s[end - 1u] == ' ')
        end--;

    if (start == end)
        return false;

    uint64_t val = 0u;
    for (uint8_t i = start; i < end; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;

        val = val * 10u + (uint64_t)(s[i] - '0');
        if (val > HID_READER_MAX_VALUE)
            return false;
    }

    if (val == 0u)
        return false;

    *out = val;
    return true;
}

static void FinishScan(void)
{
    if (s_overflow)
    {
        LOG_XATO("HID", "Kod juda uzun (>%u belgi) - tashlandi", (unsigned)HID_READER_LINE_MAX);
    }
    else if (s_len > 0u)
    {
        uint64_t value;
        s_line[s_len] = '\0';

        if (!ParseValue(s_line, s_len, &value))
            LOG_XATO("HID", "Yaroqsiz kod \"%s\" (1..%lu oralig'idagi raqam kutiladi) - tashlandi", s_line, (unsigned long)HID_READER_MAX_VALUE);
        else if (Queue_Enqueue(s_queue, HID_TYPE, value))
            LOG_INFO("HID", "QR o'qildi: %lu -> queue (%u/%u)", (unsigned long)value, Queue_Count(s_queue), (unsigned)QUEUE_MAX_ITEMS);
        else
            LOG_XATO("HID", "Queue to'la! Ma'lumot yo'qoldi: %lu (%u/%u)", (unsigned long)value, Queue_Count(s_queue), (unsigned)QUEUE_MAX_ITEMS);
    }

    ClearScan();
}

/* US QWERTY joylashuvi - skanerlar sukut bo'yicha shu joylashuvda yuboradi.
   Matn bo'lmagan tugmalar (F1, strelkalar, CapsLock...) uchun '\0'. */
static char UsageToChar(uint8_t usage, bool shift)
{
    if (usage >= 0x04u && usage <= 0x1Du)  /* a..z */
        return (char)((shift ? 'A' : 'a') + (usage - 0x04u));
    if (usage >= 0x1Eu && usage <= 0x27u)  /* 1..9, 0 */
        return shift ? "!@#$%^&*()"[usage - 0x1Eu] : "1234567890"[usage - 0x1Eu];
    if (usage >= 0x59u && usage <= 0x62u)  /* keypad 1..9, 0 */
        return "1234567890"[usage - 0x59u];

    switch (usage)
    {
    case 0x2Cu: return ' ';
    case 0x2Du: return shift ? '_' : '-';
    case 0x2Eu: return shift ? '+' : '=';
    case 0x2Fu: return shift ? '{' : '[';
    case 0x30u: return shift ? '}' : ']';
    case 0x31u: return shift ? '|' : '\\';
    case 0x33u: return shift ? ':' : ';';
    case 0x34u: return shift ? '"' : '\'';
    case 0x35u: return shift ? '~' : '`';
    case 0x36u: return shift ? '<' : ',';
    case 0x37u: return shift ? '>' : '.';
    case 0x38u: return shift ? '?' : '/';
    case 0x54u: return '/';  /* keypad */
    case 0x55u: return '*';
    case 0x56u: return '-';
    case 0x57u: return '+';
    case 0x63u: return '.';
    default:    return '\0';
    }
}

static void HandleKey(uint8_t usage, bool shift)
{
    if (usage == USAGE_ENTER || usage == USAGE_KEYPAD_ENTER || usage == USAGE_TAB)
    {
        FinishScan();
        return;
    }

    char c = UsageToChar(usage, shift);
    if (c == '\0')
        return;

    if (s_len < HID_READER_LINE_MAX)
        s_line[s_len++] = c;
    else
        s_overflow = true;
}

/* usbh_hidkbd.h da e'lon qilingan; USBH_Process() ichidan, ya'ni main loop
   kontekstida chaqiriladi - log yozish va queue ga qo'shish xavfsiz. */
void USBH_HIDKBD_ReportCallback(USBH_HandleTypeDef *phost, const USBH_HIDKBD_Report *report)
{
    (void)phost;

    /* Rollover: bir vaqtda juda ko'p tugma, massiv 0x01 bilan to'ldiriladi -
       haqiqiy holat noma'lum, hisobot o'tkazib yuboriladi. */
    if (report->keys[0] == USAGE_ERROR_ROLLOVER)
        return;

    bool shift = (report->modifiers & MODS_SHIFT) != 0u;
    /* Ctrl/Alt/GUI bilan kelgan tugma matn emas (masalan Alt+Numpad kodlari) */
    bool command = (report->modifiers & MODS_COMMAND) != 0u;

    for (uint8_t i = 0u; i < HIDKBD_MAX_KEYS; i++)
    {
        uint8_t usage = report->keys[i];
        if (usage == 0u || memchr(s_prevKeys, usage, sizeof(s_prevKeys)) != NULL)
            continue;  /* bo'sh katak yoki oldingi hisobotdan beri bosilib turibdi */

        s_lastKeyTick = HAL_GetTick();
        if (!command)
            HandleKey(usage, shift);
    }

    memcpy(s_prevKeys, report->keys, sizeof(s_prevKeys));
}

// ============== Qurilma holati ==============

static DevState CurrentDevState(void)
{
    switch (hUsbHostHS.gState)
    {
    case HOST_IDLE:
    case HOST_DEV_DISCONNECTED:
        return (hUsbHostHS.device.is_connected != 0u) ? DEV_ENUMERATING : DEV_NONE;
    case HOST_CLASS:
        return DEV_READY;
    case HOST_ABORT_STATE:
        return DEV_FAILED;
    default:
        return DEV_ENUMERATING;
    }
}

/* USBH_UserProcess() "qo'llab-quvvatlanmadi" holatini xabar qilmaydi, shuning
   uchun holat to'g'ridan-to'g'ri host state machine dan kuzatiladi. */
static void TrackDevice(void)
{
    DevState state = CurrentDevState();
    if (state == s_devState)
        return;

    const USBH_DeviceTypeDef *dev = &hUsbHostHS.device;

    switch (state)
    {
    case DEV_ENUMERATING:
        LOG_INFO("HID", "USB qurilma ulandi, enumeratsiya...");
        break;

    case DEV_READY:
        LOG_OK("HID", "QR skaner tayyor (VID=%04X PID=%04X)", dev->DevDesc.idVendor, dev->DevDesc.idProduct);
        break;

    case DEV_FAILED:
        LOG_XATO("HID", "USB qurilma ishga tushmadi (VID=%04X PID=%04X, %u interfeys)",
                 dev->DevDesc.idVendor, dev->DevDesc.idProduct, dev->CfgDesc.bNumInterfaces);
        LOG_XATO("HID", "Sababi yuqoridagi [USB] qatorlarida; ular yo'q bo'lsa - enumeratsiya xatosi. Qayta ulang");
        break;

    case DEV_NONE:
    default:
        if (s_len > 0u || s_overflow)
            LOG_XATO("HID", "Skanerlash o'rtasida uzildi - qisman kod tashlandi");
        LOG_INFO("HID", "USB qurilma uzildi");
        ClearScan();
        memset(s_prevKeys, 0, sizeof(s_prevKeys));
        break;
    }

    s_devState = state;
}

// ============== API ==============

void HidReader_Init(Queue *q)
{
    s_queue = q;
    s_devState = DEV_NONE;
    ClearScan();
    memset(s_prevKeys, 0, sizeof(s_prevKeys));
}

void HidReader_Process(void)
{
    uint32_t now = HAL_GetTick();

    TrackDevice();

    if ((now - s_lastProcessTick) > LOOP_STALL_MS)
        s_lastKeyTick = now;  /* main loop bloklangan edi - bu skanerlashdagi pauza emas */
    s_lastProcessTick = now;

    /* Suffikssiz skaner: oxirgi belgidan keyin pauza bo'lsa satr tugagan */
    if ((s_len > 0u || s_overflow) && (now - s_lastKeyTick) >= HID_READER_SCAN_TIMEOUT_MS)
        FinishScan();
}
