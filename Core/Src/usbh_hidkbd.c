/*
 * usbh_hidkbd.c - USB Host: istalgan HID klaviatura uchun class drayver
 *
 * Holat mashinasi:
 *   Init      : kamida bitta interrupt IN endpointli HID interfeys borligini
 *               tekshiradi
 *   Requests  : HID interfeyslarni birma-bir ko'rib chiqadi:
 *                 GET_DESCRIPTOR(Report, wIndex = interfeys) -> parse
 *               klaviatura topilsa: SET_IDLE -> (boot bo'lsa) SET_PROTOCOL(boot)
 *               -> IN pipe ochiladi
 *   Process   : interrupt IN polling (bInterval bo'yicha) -> dekodlash ->
 *               USBH_HIDKBD_ReportCallback()
 *
 * Nega qayta so'rov vaqt bo'yicha yuboriladi:
 *   HAL interrupt endpoint NAK olganda kanalni to'xtatadi, lekin URB holatini
 *   URB_IDLE da qoldiradi (NOTREADY emas). URB holati kutilsa birinchi NAK
 *   dan keyin polling abadiy to'xtab qolardi.
 *
 * Nega SOFProcess bo'sh:
 *   ST HID drayverida holatni SOF uzilishi ham, main loop ham o'zgartiradi -
 *   bu poygada hisobot yo'qolishi mumkin. Bu yerda hammasi main loop da,
 *   uzilishdan faqat phost->Timer o'qiladi.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#include "usbh_hidkbd.h"
#include "log.h"
#include <stdbool.h>
#include <string.h>

/* Topilgan interfeyslarning report descriptorini hex ko'rinishda logga
   chiqaradi. Yangi skaner modelini ulashda diagnostika uchun foydali. */
#define HIDKBD_LOG_REPORT_DESC  1u

#define HID_CLASS_CODE          0x03u
#define HID_SUBCLASS_BOOT       0x01u
#define HID_PROTOCOL_KEYBOARD   0x01u

#define HID_REQ_SET_IDLE        0x0Au
#define HID_REQ_SET_PROTOCOL    0x0Bu
#define HID_PROTOCOL_BOOT       0x00u   /* SET_PROTOCOL wValue: 0 = boot, 1 = report */

#define HIDKBD_MAX_REPORT       64u     /* FS interrupt endpoint MPS chegarasi = bufer */
#define HIDKBD_MAX_FIELDS       4u
#define HIDKBD_MAX_USAGES       8u
#define HIDKBD_MAX_REPORT_IDS   8u
#define HIDKBD_PUSH_DEPTH       4u

/*
 * Qayta so'rovgacha eng qisqa vaqt. HAL davriy tranzaksiyani keyingi freymga
 * rejalashtiradi va NAK dan keyin kanalni shu yoki keyingi freymda to'xtatadi -
 * 4ms kanal to'liq to'xtaganidan keyingina qayta ishga tushirishni kafolatlaydi.
 */
#define HIDKBD_MIN_POLL_MS      4u

#define PIPE_NONE               0xFFu

/* HID report descriptor elementlari (HID 1.11, 6.2.2) */
#define ITEM_LONG_PREFIX        0xFEu
#define ITEM_TYPE_MAIN          0u
#define ITEM_TYPE_GLOBAL        1u
#define ITEM_TYPE_LOCAL         2u

#define MAIN_INPUT              0x8u
#define MAIN_COLLECTION         0xAu
#define MAIN_END_COLLECTION     0xCu

#define GLOBAL_USAGE_PAGE       0x0u
#define GLOBAL_LOGICAL_MIN      0x1u
#define GLOBAL_REPORT_SIZE      0x7u
#define GLOBAL_REPORT_ID        0x8u
#define GLOBAL_REPORT_COUNT     0x9u
#define GLOBAL_PUSH             0xAu
#define GLOBAL_POP              0xBu

#define LOCAL_USAGE             0x0u
#define LOCAL_USAGE_MIN         0x1u
#define LOCAL_USAGE_MAX         0x2u

#define INPUT_CONSTANT          0x01u
#define INPUT_VARIABLE          0x02u
#define COLLECTION_APPLICATION  0x01u

#define USAGE_PAGE_DESKTOP      0x01u
#define USAGE_PAGE_KEYBOARD     0x07u
#define USAGE_DESKTOP_KEYBOARD  0x06u
#define USAGE_DESKTOP_KEYPAD    0x07u
#define USAGE_KBD_LEFT_CTRL     0xE0u
#define USAGE_KBD_RIGHT_GUI     0xE7u

// ============== Turlar ==============

/* Hisobotdagi bitta klaviatura maydoni (modifierlar yoki tugmalar massivi) */
typedef struct {
    uint16_t bitPos;                     /* Report ID baytidan keyingi boshlanish biti */
    uint8_t  bitSize;
    uint8_t  count;
    bool     isArray;                    /* true: qiymat = usage indeksi; false: har element bitta usage */
    int32_t  logicalMin;
    uint16_t usageMin;                   /* nUsages == 0 bo'lsa ishlatiladi */
    uint16_t usageMax;
    uint8_t  nUsages;                    /* aniq sanab o'tilgan usage lar */
    uint16_t usages[HIDKBD_MAX_USAGES];
} KbdField;

typedef struct {
    uint8_t  reportId;                   /* 0 = hisobotlarda Report ID bayti yo'q */
    uint8_t  nFields;
    uint16_t needBytes;                  /* maydonlar uchun kerakli payload (ID siz) */
    uint16_t maxInputBytes;              /* interfeysdagi eng uzun input hisobot (ID bilan) */
    KbdField fields[HIDKBD_MAX_FIELDS];
} KbdLayout;

typedef enum {
    REQ_SELECT_ITF,
    REQ_GET_REPORT_DESC,
    REQ_SET_IDLE,
    REQ_SET_PROTOCOL,
    REQ_START,
} ReqState;

typedef enum {
    POLL_SUBMIT,
    POLL_WAIT,
    POLL_INTERVAL,
    POLL_CLEAR_STALL,
} PollState;

typedef struct {
    ReqState         reqState;
    PollState        pollState;
    uint8_t          itfIdx;             /* Itf_Desc[] indeksi: ko'rilayotgan, keyin tanlangan */
    uint16_t         descLen;
    bool             hasLayout;          /* report descriptor muvaffaqiyatli parse qilindi */
    bool             bootProtocol;       /* SET_PROTOCOL(boot) qabul qilindi */
    KbdLayout        layout;
    const KbdLayout *active;             /* &layout yoki &kBootLayout */
    uint8_t          inEp;
    uint8_t          inPipe;
    uint8_t          xferLen;
    uint16_t         pollMs;
    uint32_t         timer;
    uint8_t          buf[HIDKBD_MAX_REPORT];
} HidKbdHandle;

/* Boot protokol hisoboti (HID 1.11, B.1): [modifierlar][rezerv][6 ta tugma] */
static const KbdLayout kBootLayout = {
    .reportId      = 0u,
    .nFields       = 2u,
    .needBytes     = 8u,
    .maxInputBytes = 8u,
    .fields = {
        { .bitPos = 0u,  .bitSize = 1u, .count = 8u, .isArray = false,
          .usageMin = USAGE_KBD_LEFT_CTRL, .usageMax = USAGE_KBD_RIGHT_GUI },
        { .bitPos = 16u, .bitSize = 8u, .count = HIDKBD_MAX_KEYS, .isArray = true,
          .logicalMin = 0, .usageMin = 0x00u, .usageMax = 0xFFu },
    },
};

_Static_assert(HIDKBD_MAX_REPORT <= 255u, "xferLen uint8_t ga sig'ishi kerak");

static HidKbdHandle s_kbd = { .inPipe = PIPE_NONE };

static USBH_StatusTypeDef HIDKBD_Init(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef HIDKBD_DeInit(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef HIDKBD_Requests(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef HIDKBD_Process(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef HIDKBD_SOFProcess(USBH_HandleTypeDef *phost);

static USBH_ClassTypeDef HIDKBD_Class = {
    "HID-KBD",
    HID_CLASS_CODE,
    HIDKBD_Init,
    HIDKBD_DeInit,
    HIDKBD_Requests,
    HIDKBD_Process,
    HIDKBD_SOFProcess,   /* yadro NULL ni tekshirmaydi - bo'sh bo'lsa ham kerak */
    NULL,
};

// ============== Report descriptor parser ==============

typedef struct {
    uint16_t usagePage;
    int32_t  logicalMin;
    uint32_t reportSize;
    uint32_t reportCount;
    uint8_t  reportId;
} Globals;

typedef struct {
    uint32_t usages[HIDKBD_MAX_USAGES];  /* 4 baytli (extended) usage da yuqori so'z = page */
    uint8_t  nUsages;
    uint32_t usageMin;
    uint32_t usageMax;
    bool     hasMin;
    bool     hasMax;
} Locals;

typedef struct {
    uint8_t  id;
    uint32_t bits;
} ReportBits;

/* Usage page Main element paytidagi global page dan olinadi, agar usage
   4 baytli (page ni o'zi ichida olib yurgan) bo'lmasa. */
static uint16_t UsagePage(uint32_t usage, const Globals *g)
{
    return ((usage >> 16) != 0u) ? (uint16_t)(usage >> 16) : g->usagePage;
}

static ReportBits *FindReportBits(ReportBits *ids, uint8_t *nIds, uint8_t id)
{
    for (uint8_t i = 0u; i < *nIds; i++)
    {
        if (ids[i].id == id)
            return &ids[i];
    }

    if (*nIds >= HIDKBD_MAX_REPORT_IDS)
        return NULL;

    ids[*nIds].id = id;
    ids[*nIds].bits = 0u;
    return &ids[(*nIds)++];
}

static void AddKbdField(KbdLayout *out, const Globals *g, const Locals *l, uint32_t bitPos, bool isArray)
{
    if (l->nUsages == 0u && !l->hasMin)
        return;  /* usage siz maydon - klaviatura ma'lumoti emas */

    uint32_t first = (l->nUsages > 0u) ? l->usages[0] : l->usageMin;
    if (UsagePage(first, g) != USAGE_PAGE_KEYBOARD)
        return;  /* masalan LED lar yoki Consumer tugmalar */

    if (out->nFields == 0u)
        out->reportId = g->reportId;

    if (g->reportId != out->reportId || out->nFields >= HIDKBD_MAX_FIELDS)
        return;

    if (g->reportSize == 0u || g->reportSize > 32u || g->reportCount == 0u || g->reportCount > 255u)
        return;

    uint32_t endBit = bitPos + g->reportSize * g->reportCount;
    uint32_t payloadBits = (HIDKBD_MAX_REPORT - ((g->reportId != 0u) ? 1u : 0u)) * 8u;
    if (endBit > payloadBits)
        return;  /* buferga sig'maydi */

    KbdField *f = &out->fields[out->nFields++];
    memset(f, 0, sizeof(*f));
    f->bitPos     = (uint16_t)bitPos;
    f->bitSize    = (uint8_t)g->reportSize;
    f->count      = (uint8_t)g->reportCount;
    f->isArray    = isArray;
    f->logicalMin = g->logicalMin;

    if (l->nUsages > 0u)
    {
        f->nUsages = l->nUsages;
        for (uint8_t i = 0u; i < l->nUsages; i++)
            f->usages[i] = (uint16_t)l->usages[i];
    }
    else
    {
        f->usageMin = (uint16_t)l->usageMin;
        f->usageMax = l->hasMax ? (uint16_t)l->usageMax : 0xFFFFu;
    }

    uint16_t need = (uint16_t)((endBit + 7u) / 8u);
    if (need > out->needBytes)
        out->needBytes = need;
}

/*
 * Keyboard/Keypad Application collection ichidagi, Keyboard usage page dagi
 * input maydonlarini yig'adi. Qurilmadan kelgan ma'lumot ishonchsiz deb
 * qaraladi: har o'qish chegara bilan tekshiriladi, buzuq descriptor da false.
 */
static bool ParseReportDescriptor(const uint8_t *d, uint16_t len, KbdLayout *out)
{
    Globals    g = {0};
    Globals    stack[HIDKBD_PUSH_DEPTH];
    uint8_t    sp = 0u;
    Locals     l = {0};
    ReportBits ids[HIDKBD_MAX_REPORT_IDS];
    uint8_t    nIds = 0u;
    bool       usesIds = false;
    uint8_t    depth = 0u;
    uint8_t    kbdDepth = 0u;    /* != 0: klaviatura collection ichida (ochilgandan keyingi chuqurlik) */
    bool       kbdDone = false;  /* birinchi klaviatura collection yakunlandi */
    uint16_t   pos = 0u;

    memset(out, 0, sizeof(*out));

    while (pos < len)
    {
        uint8_t prefix = d[pos++];

        if (prefix == ITEM_LONG_PREFIX)
        {
            /* Long item: [bDataSize][bLongItemTag][data] - HID da ishlatilmaydi, o'tkazamiz */
            if ((uint32_t)pos + 2u > len)
                return false;
            pos = (uint16_t)(pos + 2u + d[pos]);
            continue;
        }

        uint8_t size = prefix & 0x03u;
        uint8_t type = (prefix >> 2) & 0x03u;
        uint8_t tag  = (prefix >> 4) & 0x0Fu;
        if (size == 3u)
            size = 4u;

        if ((uint32_t)pos + size > len)
            return false;

        uint32_t uval = 0u;
        for (uint8_t i = 0u; i < size; i++)
            uval |= (uint32_t)d[pos + i] << (8u * i);
        pos = (uint16_t)(pos + size);

        int32_t sval = (int32_t)uval;
        if (size == 1u)
            sval = (int8_t)uval;
        else if (size == 2u)
            sval = (int16_t)uval;

        if (type == ITEM_TYPE_MAIN)
        {
            if (tag == MAIN_COLLECTION)
            {
                if (kbdDepth == 0u && !kbdDone && uval == COLLECTION_APPLICATION)
                {
                    uint32_t usage = (l.nUsages > 0u) ? l.usages[0] : (l.hasMin ? l.usageMin : 0u);
                    uint16_t id = (uint16_t)usage;
                    if (UsagePage(usage, &g) == USAGE_PAGE_DESKTOP &&
                        (id == USAGE_DESKTOP_KEYBOARD || id == USAGE_DESKTOP_KEYPAD))
                        kbdDepth = (uint8_t)(depth + 1u);
                }
                if (depth == 0xFFu)
                    return false;
                depth++;
            }
            else if (tag == MAIN_END_COLLECTION)
            {
                if (depth == 0u)
                    return false;
                if (kbdDepth != 0u && depth == kbdDepth)
                {
                    kbdDepth = 0u;
                    kbdDone = (out->nFields > 0u);
                }
                depth--;
            }
            else if (tag == MAIN_INPUT)
            {
                if (g.reportSize > 32u || g.reportCount > 4096u)
                    return false;

                ReportBits *rb = FindReportBits(ids, &nIds, g.reportId);
                if (rb == NULL)
                    return false;

                if (kbdDepth != 0u && (uval & INPUT_CONSTANT) == 0u)
                    AddKbdField(out, &g, &l, rb->bits, (uval & INPUT_VARIABLE) == 0u);

                rb->bits += g.reportSize * g.reportCount;
            }

            /* Local elementlar har Main elementdan keyin tozalanadi (Output/Feature ham) */
            memset(&l, 0, sizeof(l));
        }
        else if (type == ITEM_TYPE_GLOBAL)
        {
            switch (tag)
            {
            case GLOBAL_USAGE_PAGE:   g.usagePage = (uint16_t)uval; break;
            case GLOBAL_LOGICAL_MIN:  g.logicalMin = sval; break;
            case GLOBAL_REPORT_SIZE:  g.reportSize = uval; break;
            case GLOBAL_REPORT_COUNT: g.reportCount = uval; break;
            case GLOBAL_REPORT_ID:
                if (uval == 0u || uval > 0xFFu)
                    return false;
                g.reportId = (uint8_t)uval;
                usesIds = true;
                break;
            case GLOBAL_PUSH:
                if (sp >= HIDKBD_PUSH_DEPTH)
                    return false;
                stack[sp++] = g;
                break;
            case GLOBAL_POP:
                if (sp == 0u)
                    return false;
                g = stack[--sp];
                break;
            default:
                break;
            }
        }
        else if (type == ITEM_TYPE_LOCAL)
        {
            switch (tag)
            {
            case LOCAL_USAGE:
                if (l.nUsages < HIDKBD_MAX_USAGES)
                    l.usages[l.nUsages++] = uval;
                break;
            case LOCAL_USAGE_MIN: l.usageMin = uval; l.hasMin = true; break;
            case LOCAL_USAGE_MAX: l.usageMax = uval; l.hasMax = true; break;
            default: break;
            }
        }
    }

    if (out->nFields == 0u)
        return false;

    /* Report ID ishlatilsa hamma hisobotlar ID bilan boshlanadi - klaviatura
       maydonlari ID siz qolgan bo'lsa descriptor buzuq, baytlar siljib ketardi. */
    if (usesIds && out->reportId == 0u)
        return false;

    uint32_t maxBytes = 0u;
    for (uint8_t i = 0u; i < nIds; i++)
    {
        uint32_t bytes = (ids[i].bits + 7u) / 8u + (usesIds ? 1u : 0u);
        if (bytes > maxBytes)
            maxBytes = bytes;
    }
    out->maxInputBytes = (uint16_t)((maxBytes > 0xFFFFu) ? 0xFFFFu : maxBytes);

    return true;
}

// ============== Hisobotni dekodlash ==============

static uint32_t ReadBits(const uint8_t *buf, uint16_t bitPos, uint8_t bitSize)
{
    uint32_t v = 0u;
    for (uint8_t i = 0u; i < bitSize; i++)
    {
        uint16_t b = (uint16_t)(bitPos + i);
        if (((buf[b >> 3] >> (b & 7u)) & 1u) != 0u)
            v |= 1uL << i;
    }
    return v;
}

static uint16_t ArrayUsage(const KbdField *f, uint32_t index)
{
    if (f->nUsages > 0u)
        return (index < f->nUsages) ? f->usages[index] : 0u;

    uint32_t usage = (uint32_t)f->usageMin + index;
    return (usage <= f->usageMax) ? (uint16_t)usage : 0u;
}

static uint16_t VariableUsage(const KbdField *f, uint32_t index)
{
    /* HID 1.11: usage lar elementlardan kam bo'lsa oxirgisi qolganlarga ham tegishli */
    if (f->nUsages > 0u)
        return f->usages[(index < f->nUsages) ? index : (f->nUsages - 1u)];

    uint32_t usage = (uint32_t)f->usageMin + index;
    return (usage <= f->usageMax) ? (uint16_t)usage : 0u;
}

static void DecodeReport(USBH_HandleTypeDef *phost, const KbdLayout *lay, const uint8_t *data, uint32_t len)
{
    if (lay->reportId != 0u)
    {
        if (len == 0u || data[0] != lay->reportId)
            return;  /* shu interfeysdagi boshqa hisobot (masalan media tugmalar) */
        data++;
        len--;
    }

    if (len < lay->needBytes)
        return;  /* qisqa hisobot - maydonlarni o'qish chegaradan chiqardi */

    USBH_HIDKBD_Report report;
    uint8_t nKeys = 0u;
    memset(&report, 0, sizeof(report));

    for (uint8_t fi = 0u; fi < lay->nFields; fi++)
    {
        const KbdField *f = &lay->fields[fi];

        for (uint16_t i = 0u; i < f->count; i++)
        {
            uint32_t value = ReadBits(data, (uint16_t)(f->bitPos + i * f->bitSize), f->bitSize);
            uint16_t usage;

            if (f->isArray)
            {
                int64_t index = (int64_t)value - f->logicalMin;
                if (index < 0)
                    continue;
                usage = ArrayUsage(f, (uint32_t)index);
            }
            else
            {
                if (value == 0u)
                    continue;
                usage = VariableUsage(f, i);
            }

            if (usage >= USAGE_KBD_LEFT_CTRL && usage <= USAGE_KBD_RIGHT_GUI)
                report.modifiers |= (uint8_t)(1u << (usage - USAGE_KBD_LEFT_CTRL));
            else if (usage != 0u && usage <= 0xFFu && nKeys < HIDKBD_MAX_KEYS)
                report.keys[nKeys++] = (uint8_t)usage;
        }
    }

    USBH_HIDKBD_ReportCallback(phost, &report);
}

// ============== Yordamchilar ==============

static const USBH_InterfaceDescTypeDef *Itf(const USBH_HandleTypeDef *phost, uint8_t idx)
{
    return &phost->device.CfgDesc.Itf_Desc[idx];
}

/* ST faqat USBH_MAX_NUM_INTERFACES tagacha interfeysni parse qiladi */
static uint8_t ParsedInterfaceCount(const USBH_HandleTypeDef *phost)
{
    uint8_t n = phost->device.CfgDesc.bNumInterfaces;
    return (n < USBH_MAX_NUM_INTERFACES) ? n : (uint8_t)USBH_MAX_NUM_INTERFACES;
}

static const USBH_EpDescTypeDef *FindInterruptIn(const USBH_InterfaceDescTypeDef *itf)
{
    uint8_t n = (itf->bNumEndpoints < USBH_MAX_NUM_ENDPOINTS) ? itf->bNumEndpoints : (uint8_t)USBH_MAX_NUM_ENDPOINTS;

    for (uint8_t i = 0u; i < n; i++)
    {
        const USBH_EpDescTypeDef *ep = &itf->Ep_Desc[i];
        if ((ep->bEndpointAddress & 0x80u) != 0u && (ep->bmAttributes & 0x03u) == USB_EP_TYPE_INTR)
            return ep;
    }
    return NULL;
}

static bool IsHidCandidate(const USBH_HandleTypeDef *phost, uint8_t idx)
{
    const USBH_InterfaceDescTypeDef *itf = Itf(phost, idx);
    return itf->bInterfaceClass == HID_CLASS_CODE && FindInterruptIn(itf) != NULL;
}

static bool IsBootKeyboard(const USBH_InterfaceDescTypeDef *itf)
{
    return itf->bInterfaceSubClass == HID_SUBCLASS_BOOT && itf->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD;
}

/* Konfiguratsiya descriptorida idx-interfeysga tegishli HID descriptordan
   report descriptor uzunligini oladi; topilmasa 0. */
static uint16_t ReportDescLength(const USBH_HandleTypeDef *phost, uint8_t idx)
{
    const uint8_t *cfg = phost->device.CfgDesc_Raw;
    uint16_t total = phost->device.CfgDesc.wTotalLength;
    uint16_t pos = 0u;
    int16_t  itf = -1;

    while ((uint32_t)pos + 2u <= total)
    {
        uint8_t len  = cfg[pos];
        uint8_t type = cfg[pos + 1u];

        if (len < 2u || (uint32_t)pos + len > total)
            break;

        if (type == USB_DESC_TYPE_INTERFACE)
        {
            itf++;
            if (itf > (int16_t)idx)
                break;
        }
        else if (type == USB_DESC_TYPE_HID && itf == (int16_t)idx &&
                 len >= USB_HID_DESC_SIZE && cfg[pos + 6u] == USB_DESC_TYPE_HID_REPORT)
        {
            return LE16(&cfg[pos + 7u]);
        }

        pos = (uint16_t)(pos + len);
    }
    return 0u;
}

/* USBH_GetDescriptor va ST ning HID so'rovlari wIndex ni doim 0 qiladi -
   bu yerda so'rov aynan kerakli interfeysga yuboriladi. */
static USBH_StatusTypeDef InterfaceRequest(USBH_HandleTypeDef *phost, uint8_t bmRequestType, uint8_t bRequest,
                                           uint16_t wValue, uint8_t itfNumber, uint8_t *buf, uint16_t len)
{
    if (phost->RequestState == CMD_SEND)
    {
        phost->Control.setup.b.bmRequestType = bmRequestType;
        phost->Control.setup.b.bRequest      = bRequest;
        phost->Control.setup.b.wValue.w      = wValue;
        phost->Control.setup.b.wIndex.w      = itfNumber;
        phost->Control.setup.b.wLength.w     = len;
    }
    return USBH_CtlReq(phost, buf, len);
}

static void ReleasePipe(USBH_HandleTypeDef *phost, HidKbdHandle *h)
{
    if (h->inPipe != PIPE_NONE)
    {
        (void)USBH_ClosePipe(phost, h->inPipe);
        (void)USBH_FreePipe(phost, h->inPipe);
        h->inPipe = PIPE_NONE;
    }
}

#if HIDKBD_LOG_REPORT_DESC
static void LogReportDescriptor(uint8_t itfNumber, const uint8_t *d, uint16_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[16u * 3u + 1u];

    LOG_INFO("USB", "Interfeys #%u report descriptor (%u bayt):", itfNumber, len);
    for (uint16_t i = 0u; i < len; i += 16u)
    {
        uint16_t n = (uint16_t)(len - i);
        if (n > 16u)
            n = 16u;
        uint8_t k = 0u;
        for (uint16_t j = 0u; j < n; j++)
        {
            line[k++] = ' ';
            line[k++] = hex[d[i + j] >> 4];
            line[k++] = hex[d[i + j] & 0x0Fu];
        }
        line[k] = '\0';
        LOG_INFO("USB", " %s", line);
    }
}
#endif

static USBH_StatusTypeDef StartPolling(USBH_HandleTypeDef *phost, HidKbdHandle *h)
{
    const USBH_InterfaceDescTypeDef *itf = Itf(phost, h->itfIdx);
    const USBH_EpDescTypeDef *ep = FindInterruptIn(itf);  /* IsHidCandidate kafolatlagan */

    h->active = (h->bootProtocol || !h->hasLayout) ? &kBootLayout : &h->layout;

    uint16_t mps = ep->wMaxPacketSize & 0x07FFu;
    if (mps == 0u || mps > HIDKBD_MAX_REPORT)
        mps = HIDKBD_MAX_REPORT;

    /* Bitta URB = bitta hisobot: uzunlik MPS ga karrali bo'lishi shart, aks
       holda HAL oxirgi to'liq paketdan keyin ham ma'lumot kutadi. */
    uint16_t packets = (uint16_t)((h->active->maxInputBytes + mps - 1u) / mps);
    if (packets == 0u)
        packets = 1u;
    if (packets * mps > HIDKBD_MAX_REPORT)
        packets = (uint16_t)(HIDKBD_MAX_REPORT / mps);
    h->xferLen = (uint8_t)(packets * mps);

    /* Davriy kanal har paket uchun juft/toq freymni kutadi - qo'shimcha paketga 2ms */
    h->pollMs = (uint16_t)(((ep->bInterval > HIDKBD_MIN_POLL_MS) ? ep->bInterval : HIDKBD_MIN_POLL_MS) + 2u * (packets - 1u));

    h->inEp = ep->bEndpointAddress;
    h->inPipe = USBH_AllocPipe(phost, h->inEp);
    if (h->inPipe == PIPE_NONE)
    {
        LOG_XATO("USB", "Bo'sh pipe qolmadi - klaviatura ochilmadi");
        return USBH_FAIL;
    }

    (void)USBH_OpenPipe(phost, h->inPipe, h->inEp, phost->device.address, phost->device.speed, USB_EP_TYPE_INTR, mps);
    (void)USBH_LL_SetToggle(phost, h->inPipe, 0u);
    (void)USBH_SelectInterface(phost, h->itfIdx);

    if (h->active == &kBootLayout)
        LOG_OK("USB", "Klaviatura: interfeys #%u, boot format, EP 0x%02X, %u bayt, poll %ums",
               itf->bInterfaceNumber, h->inEp, h->xferLen, h->pollMs);
    else
        LOG_OK("USB", "Klaviatura: interfeys #%u, Report ID %u, %u maydon, EP 0x%02X, %u bayt, poll %ums",
               itf->bInterfaceNumber, h->layout.reportId, h->layout.nFields, h->inEp, h->xferLen, h->pollMs);

    h->pollState = POLL_SUBMIT;
    phost->pUser(phost, HOST_USER_CLASS_ACTIVE);
    return USBH_OK;
}

// ============== Class callbacklari ==============

static USBH_StatusTypeDef HIDKBD_Init(USBH_HandleTypeDef *phost)
{
    /* Control xatosidan keyingi qayta enumeratsiyada yadro DeInit ni
       chaqirmaydi - eski pipe shu yerda bo'shatiladi, aks holda sizib ketadi. */
    ReleasePipe(phost, &s_kbd);
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_kbd.inPipe = PIPE_NONE;
    phost->pActiveClass->pData = &s_kbd;

    if (phost->device.CfgDesc.bNumInterfaces > USBH_MAX_NUM_INTERFACES)
        LOG_INFO("USB", "Qurilmada %u interfeys, faqat birinchi %u tasi ko'riladi (USBH_MAX_NUM_INTERFACES)",
                 phost->device.CfgDesc.bNumInterfaces, (unsigned)USBH_MAX_NUM_INTERFACES);

    for (uint8_t i = 0u; i < ParsedInterfaceCount(phost); i++)
    {
        if (IsHidCandidate(phost, i))
            return USBH_OK;
    }

    LOG_XATO("USB", "Interrupt IN endpointli HID interfeys yo'q");
    return USBH_FAIL;
}

static USBH_StatusTypeDef HIDKBD_DeInit(USBH_HandleTypeDef *phost)
{
    ReleasePipe(phost, &s_kbd);
    phost->pActiveClass->pData = NULL;
    return USBH_OK;
}

/*
 * Control so'rov natijalari:
 *   USBH_OK            - bajarildi
 *   USBH_NOT_SUPPORTED - qurilma STALL qildi (so'rov qo'llab-quvvatlanmaydi)
 *   USBH_FAIL          - qurilma javob bermadi; yadro o'zi gState = HOST_IDLE
 *                        qilib qayta enumeratsiya boshlaydi. Bu yerda BUSY
 *                        qaytariladi - FAIL qaytarilsa yadro buni ABORT bilan
 *                        almashtirib, qayta urinishni to'xtatib qo'yardi.
 */
static USBH_StatusTypeDef HIDKBD_Requests(USBH_HandleTypeDef *phost)
{
    HidKbdHandle *h = (HidKbdHandle *)phost->pActiveClass->pData;
    const USBH_InterfaceDescTypeDef *itf;
    USBH_StatusTypeDef status = USBH_BUSY;
    USBH_StatusTypeDef req;

    switch (h->reqState)
    {
    case REQ_SELECT_ITF:
        while (h->itfIdx < ParsedInterfaceCount(phost) && !IsHidCandidate(phost, h->itfIdx))
            h->itfIdx++;

        if (h->itfIdx >= ParsedInterfaceCount(phost))
        {
            LOG_XATO("USB", "Klaviatura interfeysi topilmadi (%u interfeys ko'rildi)", ParsedInterfaceCount(phost));
            status = USBH_FAIL;
            break;
        }

        itf = Itf(phost, h->itfIdx);
        h->descLen = ReportDescLength(phost, h->itfIdx);
        if (h->descLen > sizeof(phost->device.Data))
        {
            LOG_INFO("USB", "Interfeys #%u report descriptor %u bayt, %u gacha o'qiladi",
                     itf->bInterfaceNumber, h->descLen, (unsigned)sizeof(phost->device.Data));
            h->descLen = sizeof(phost->device.Data);
        }

        if (h->descLen == 0u)
        {
            /* HID descriptor yo'q: formatni faqat boot klaviaturada bilamiz */
            if (IsBootKeyboard(itf))
            {
                h->hasLayout = false;
                h->reqState = REQ_SET_IDLE;
            }
            else
            {
                h->itfIdx++;
            }
            break;
        }

        memset(phost->device.Data, 0, h->descLen);
        h->reqState = REQ_GET_REPORT_DESC;
        break;

    case REQ_GET_REPORT_DESC:
        itf = Itf(phost, h->itfIdx);
        req = InterfaceRequest(phost, USB_D2H | USB_REQ_RECIPIENT_INTERFACE | USB_REQ_TYPE_STANDARD,
                               USB_REQ_GET_DESCRIPTOR, USB_DESC_HID_REPORT, itf->bInterfaceNumber,
                               phost->device.Data, h->descLen);
        if (req == USBH_BUSY || req == USBH_FAIL)
            break;

#if HIDKBD_LOG_REPORT_DESC
        if (req == USBH_OK)
            LogReportDescriptor(itf->bInterfaceNumber, phost->device.Data, h->descLen);
#endif

        h->hasLayout = (req == USBH_OK) && ParseReportDescriptor(phost->device.Data, h->descLen, &h->layout);

        if (h->hasLayout || IsBootKeyboard(itf))
        {
            h->reqState = REQ_SET_IDLE;
        }
        else
        {
            LOG_INFO("USB", "Interfeys #%u (%02X/%02X/%02X) klaviatura emas - o'tkazildi",
                     itf->bInterfaceNumber, itf->bInterfaceClass, itf->bInterfaceSubClass, itf->bInterfaceProtocol);
            h->itfIdx++;
            h->reqState = REQ_SELECT_ITF;
        }
        break;

    case REQ_SET_IDLE:
        itf = Itf(phost, h->itfIdx);
        /* Idle rate 0: hisobot faqat holat o'zgarganda keladi */
        req = InterfaceRequest(phost, USB_H2D | USB_REQ_RECIPIENT_INTERFACE | USB_REQ_TYPE_CLASS,
                               HID_REQ_SET_IDLE, 0u, itf->bInterfaceNumber, NULL, 0u);
        if (req == USBH_OK || req == USBH_NOT_SUPPORTED)  /* SET_IDLE ixtiyoriy */
            h->reqState = IsBootKeyboard(itf) ? REQ_SET_PROTOCOL : REQ_START;
        break;

    case REQ_SET_PROTOCOL:
        itf = Itf(phost, h->itfIdx);
        /* Boot protokolda format standart - descriptor parse ga bog'liq emas */
        req = InterfaceRequest(phost, USB_H2D | USB_REQ_RECIPIENT_INTERFACE | USB_REQ_TYPE_CLASS,
                               HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, itf->bInterfaceNumber, NULL, 0u);
        if (req == USBH_OK || req == USBH_NOT_SUPPORTED)
        {
            h->bootProtocol = (req == USBH_OK);
            h->reqState = REQ_START;
        }
        break;

    case REQ_START:
    default:
        status = StartPolling(phost, h);
        break;
    }

    return status;
}

static USBH_StatusTypeDef HIDKBD_Process(USBH_HandleTypeDef *phost)
{
    HidKbdHandle *h = (HidKbdHandle *)phost->pActiveClass->pData;
    USBH_StatusTypeDef req;

    switch (h->pollState)
    {
    case POLL_SUBMIT:
        (void)USBH_InterruptReceiveData(phost, h->buf, h->xferLen, h->inPipe);
        h->timer = phost->Timer;
        h->pollState = POLL_WAIT;
        break;

    case POLL_WAIT:
        switch (USBH_LL_GetURBState(phost, h->inPipe))
        {
        case USBH_URB_DONE:
        {
            uint32_t n = USBH_LL_GetLastXferSize(phost, h->inPipe);
            if (n > h->xferLen)
                n = h->xferLen;
            if (n > 0u)
                DecodeReport(phost, h->active, h->buf, n);
            h->pollState = POLL_INTERVAL;  /* URB_DONE qayta o'qilmasin */
            break;
        }

        case USBH_URB_STALL:
            h->pollState = POLL_CLEAR_STALL;
            break;

        default:
            /* NAK yoki uzatish xatosi: kanal to'xtatilgan, URB holati IDLE qolishi mumkin */
            if ((phost->Timer - h->timer) >= h->pollMs)
                h->pollState = POLL_SUBMIT;
            break;
        }
        break;

    case POLL_INTERVAL:
        if ((phost->Timer - h->timer) >= h->pollMs)
            h->pollState = POLL_SUBMIT;
        break;

    case POLL_CLEAR_STALL:
    default:
        req = USBH_ClrFeature(phost, h->inEp);
        if (req == USBH_OK || req == USBH_NOT_SUPPORTED)
        {
            (void)USBH_LL_SetToggle(phost, h->inPipe, 0u);  /* CLEAR_FEATURE toggle ni DATA0 ga qaytaradi */
            h->timer = phost->Timer;
            h->pollState = POLL_INTERVAL;
        }
        break;
    }

    return USBH_OK;
}

static USBH_StatusTypeDef HIDKBD_SOFProcess(USBH_HandleTypeDef *phost)
{
    (void)phost;
    return USBH_OK;
}

// ============== API ==============

USBH_StatusTypeDef USBH_HIDKBD_Install(USBH_HandleTypeDef *phost)
{
    for (uint32_t i = 0u; i < phost->ClassNumber; i++)
    {
        if (phost->pClass[i] != NULL && phost->pClass[i]->ClassCode == HID_CLASS_CODE)
        {
            phost->pClass[i] = &HIDKBD_Class;
            return USBH_OK;
        }
    }

    return USBH_RegisterClass(phost, &HIDKBD_Class);
}
