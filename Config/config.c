/*
 * config.c - QR buyruqlari bilan boshqariladigan sozlamalar va ularni
 *            flash da saqlash
 *
 * Saqlash joyi: FLASH SECTOR 7 (0x08060000, 128 KB) - 512 KB li F407VE ning
 * oxirgi sektori. Proshivka hozir ~187 KB (CCMRAM init ma'lumoti bilan birga)
 * egallaydi, ya'ni sektor 6 va 7 bo'sh. Agar kod kelajakda 384 KB dan oshsa
 * linker shu sektorga chiqadi va sozlamalar ustiga yoziladi - o'shanda
 * linker script ga alohida region kesib berish kerak bo'ladi.
 *
 * DIQQAT: sektorni o'chirish ~1-2 soniya davom etadi va shu vaqt ichida CPU
 * flash dan buyruq o'qiy olmaydi - main loop ham, uzilishlar ham to'xtab
 * turadi. Shuning uchun yozish faqat foydalanuvchi config QR ni skanerlaganda
 * bajariladi, ya'ni ataylab va kamdan-kam. Shu payt kelgan Wiegand impulsi
 * yo'qolishi mumkin.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#include "config.h"
#include "main.h"   /* LED3_Pin, HAL */
#include "log.h"
#include "lwip/dns.h"
#include <string.h>

/* ================= Flash dagi yozuv ================= */

#define CONFIG_FLASH_SECTOR   FLASH_SECTOR_7
#define CONFIG_FLASH_ADDR     0x08060000u

/* "CFG1" - formatni tanib olish uchun. Struktura o'zgarsa oxirgi raqam
   oshiriladi, shunda eski yozuv avtomatik rad etilib default ishlatiladi. */
#define CONFIG_MAGIC          0x31474643u

/* Bitta config QR ining eng uzun ko'rinishi:
   "#SRV=255.255.255.255:65535" = 26 belgi */
#define CONFIG_CMD_MAX        64u

/*
 * Word (4 bayt) ga tekislangan: magic(4) version(1) debug(1) port(2)
 * ip[64] resolved_ip(4) crc(4) = 80 bayt = 20 word. HAL_FLASH_Program() word
 * bilan yozadi, shuning uchun o'lcham 4 ga karrali bo'lishi shart.
 * resolved_ip: DNS natijasi (0 = unresolved, yoki IP uint32_t).
 */
typedef struct
{
    uint32_t magic;
    uint8_t  version;
    uint8_t  debug;
    uint16_t port;
    char     ip[CONFIG_IP_STR_MAX];
    uint32_t resolved_ip;               /* DNS query natijasi (0 = unresolved) */
    uint32_t crc;                       /* undan oldingi hamma bayt ustidan */
} ConfigBlob;

#define CONFIG_CRC_LEN   (sizeof(ConfigBlob) - sizeof(uint32_t))

/* HAL_FLASH_Program() word bilan yozadi, shuning uchun o'lcham 4 ga karrali
   bo'lishi SHART. Struktura kengaytirilsa shu yerda build to'xtaydi. */
_Static_assert((sizeof(ConfigBlob) % 4u) == 0u,
               "ConfigBlob o'lchami 4 ga karrali bo'lishi kerak");

static ConfigBlob s_cfg;
static uint32_t dns_timeout_start = 0u;  /* DNS query boshlangan vaqti */

#define DNS_TIMEOUT_MS  3000u             /* 3 soniya: DNS javob bermasa TCP o'tadi */

/* ================= DNS Callback ================= */

static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)arg;  /* unused */

    if (ipaddr != NULL)
    {
        s_cfg.resolved_ip = ipaddr->addr;
        LOG_OK("DNS", "Resolved: %s -> %u.%u.%u.%u",
               name,
               ip4_addr1(ipaddr), ip4_addr2(ipaddr),
               ip4_addr3(ipaddr), ip4_addr4(ipaddr));
    }
    else
    {
        LOG_XATO("DNS", "Failed to resolve: %s", name);
        s_cfg.resolved_ip = 0u;
    }
}

/* ================= CRC32 ================= */

/*
 * Standart reflected CRC-32 (polinom 0xEDB88320), jadvalsiz.
 * Apparat CRC bloki ishlatilmadi: u CubeMX da yoqilmagan va boshqacha
 * (reflecsiyasiz) hisoblaydi, 28 bayt uchun esa dasturiy variant ham
 * bir necha mikrosekund oladi.
 */
static uint32_t Crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    while (len-- > 0u)
    {
        crc ^= *p++;
        for (uint8_t bit = 0u; bit < 8u; bit++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }

    return ~crc;
}

/* ================= LED bilan tasdiq ================= */

/*
 * COM ulanmagan holatda buyruq qabul qilinganini boshqa yo'l bilan bilib
 * bo'lmaydi, shuning uchun LED3 bilan javob qaytariladi:
 *   2 marta - bajarildi va saqlandi
 *   5 marta - buyruq tanilmadi yoki format xato
 * LED lar active-low (Log_Panic() dagi bilan bir xil mantiq).
 */
static void ConfigBlink(uint8_t times)
{
    __HAL_RCC_GPIOE_CLK_ENABLE();

    for (uint8_t i = 0u; i < times; i++)
    {
        HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET);
        HAL_Delay(120u);
        HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET);
        HAL_Delay(120u);
    }
}

/* ================= Zavod qiymatlari ================= */

static void LoadDefaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    s_cfg.magic   = CONFIG_MAGIC;
    s_cfg.version = 1u;
    s_cfg.debug   = CONFIG_DEFAULT_DEBUG ? 1u : 0u;
    s_cfg.port    = CONFIG_PROD_SERVER_PORT;
    strncpy(s_cfg.ip, CONFIG_PROD_SERVER_IP, CONFIG_IP_STR_MAX - 1u);
}

/* ================= Flash ga yozish ================= */

static bool SaveToFlash(void)
{
    const uint32_t *words = (const uint32_t *)(const void *)&s_cfg;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sectorError = 0u;
    bool ok = true;

    s_cfg.crc = Crc32(&s_cfg, CONFIG_CRC_LEN);

    HAL_FLASH_Unlock();

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;   /* 2.7..3.6 V - word yozish */
    erase.Sector       = CONFIG_FLASH_SECTOR;
    erase.NbSectors    = 1u;

    if (HAL_FLASHEx_Erase(&erase, &sectorError) != HAL_OK)
    {
        ok = false;
    }
    else
    {
        for (uint32_t i = 0u; i < (sizeof(ConfigBlob) / 4u); i++)
        {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                  CONFIG_FLASH_ADDR + (i * 4u),
                                  words[i]) != HAL_OK)
            {
                ok = false;
                break;
            }
        }
    }

    HAL_FLASH_Lock();

    if (!ok)
    {
        LOG_XATO("CFG", "Flash ga yozib bo'lmadi (err=0x%lX)",
                 (unsigned long)HAL_FLASH_GetError());
    }

    return ok;
}

/* ================= Ishga tushirish ================= */

void Config_Init(void)
{
    const ConfigBlob *stored = (const ConfigBlob *)(const void *)CONFIG_FLASH_ADDR;

    /* O'chirilgan flash 0xFF bilan to'la, ya'ni magic mos kelmaydi va
       birinchi yoqilishda ham shu shart ishlaydi */
    if (stored->magic == CONFIG_MAGIC &&
        stored->version == 1u &&
        stored->crc == Crc32(stored, CONFIG_CRC_LEN))
    {
        memcpy(&s_cfg, stored, sizeof(s_cfg));

        /* ip maydoni har doim yakunlangan bo'lsin - buzilgan yozuv
           strlen() ni bufer tashqarisiga olib chiqmasin */
        s_cfg.ip[CONFIG_IP_STR_MAX - 1u] = '\0';

        LOG_OK("CFG", "Sozlamalar flash dan o'qildi");
    }
    else
    {
        LoadDefaults();
        LOG_INFO("CFG", "Saqlangan sozlama yo'q - zavod qiymatlari");
    }

    LOG_INFO("CFG", "Server: %s:%u | Debug: %s",
             s_cfg.ip, (unsigned)s_cfg.port, s_cfg.debug ? "YOQIQ" : "O'CHIQ");
}

/* ================= Buyruq tahlili ================= */

/* Domen yoki IP address? Agar faqat raqam va nuqta bo'lsa IP. */
static bool IsIpAddress(const char *s)
{
    if (!s || !*s)
        return false;

    while (*s)
    {
        if ((*s < '0' || *s > '9') && *s != '.')
            return false;
        s++;
    }

    return true;
}

/* "192.168.1.50" -> to'g'ri bo'lsa true. Nuqta bilan ajratilgan 4 ta
   0..255 oralig'idagi son, oldida nol bo'lishi mumkin ("010" = 10). */
static bool ParseIp(const char *s)
{
    uint8_t octets = 0u;

    while (octets < 4u)
    {
        uint16_t val = 0u;
        uint8_t  digits = 0u;

        while (*s >= '0' && *s <= '9')
        {
            val = (uint16_t)(val * 10u + (uint16_t)(*s - '0'));
            if (val > 255u || ++digits > 3u)
                return false;
            s++;
        }

        if (digits == 0u)
            return false;

        octets++;

        if (octets < 4u)
        {
            if (*s != '.')
                return false;
            s++;
        }
    }

    return (*s == '\0');
}

/* "5488" -> 1..65535 oralig'ida bo'lsa true */
static bool ParsePort(const char *s, uint16_t *out)
{
    uint32_t val = 0u;
    uint8_t  digits = 0u;

    while (*s >= '0' && *s <= '9')
    {
        val = val * 10u + (uint32_t)(*s - '0');
        if (val > 65535u || ++digits > 5u)
            return false;
        s++;
    }

    if (*s != '\0' || digits == 0u || val == 0u)
        return false;

    *out = (uint16_t)val;
    return true;
}

/* #SRV= dan keyingi qismni ishlaydi. arg - mahalliy nusxa, o'zgartirish mumkin. */
static bool ApplyServer(char *arg)
{
    char *colon;
    uint16_t port;
    ip_addr_t addr;

    if (strcmp(arg, "PROD") == 0)
    {
        strncpy(s_cfg.ip, CONFIG_PROD_SERVER_IP, CONFIG_IP_STR_MAX - 1u);
        s_cfg.ip[CONFIG_IP_STR_MAX - 1u] = '\0';
        s_cfg.port = CONFIG_PROD_SERVER_PORT;
        s_cfg.resolved_ip = 0u;  /* zarurat bo'lsa DNS resolve qiladi */
        return true;
    }

    if (strcmp(arg, "TEST") == 0)
    {
        strncpy(s_cfg.ip, CONFIG_TEST_SERVER_IP, CONFIG_IP_STR_MAX - 1u);
        s_cfg.ip[CONFIG_IP_STR_MAX - 1u] = '\0';
        s_cfg.port = CONFIG_TEST_SERVER_PORT;
        s_cfg.resolved_ip = 0u;
        return true;
    }

    /* <ip_yoki_domen>:<port> - ikkisi ham majburiy */
    colon = strchr(arg, ':');
    if (colon == NULL)
        return false;

    *colon = '\0';
    if (!ParsePort(colon + 1u, &port))
        return false;

    if (strlen(arg) >= CONFIG_IP_STR_MAX)
        return false;

    /* IP addressmi yoki domen? */
    if (IsIpAddress(arg))
    {
        /* IP address: darhol parse qil */
        if (!ParseIp(arg))
            return false;
        ipaddr_aton(arg, &addr);
        s_cfg.resolved_ip = addr.addr;
        LOG_OK("SRV", "IP: %s", arg);
    }
    else
    {
        /* Domen: async resolve qil */
        s_cfg.resolved_ip = 0u;  /* belgi: unresolved */
        dns_timeout_start = HAL_GetTick();
        dns_gethostbyname(arg, NULL, dns_callback, NULL);
        LOG_INFO("SRV", "Resolving domain: %s (timeout %ums)", arg, DNS_TIMEOUT_MS);
    }

    strcpy(s_cfg.ip, arg);
    s_cfg.port = port;
    return true;
}

bool Config_TryCommand(const char *line, uint8_t len)
{
    char cmd[CONFIG_CMD_MAX + 1u];
    bool applied = false;

    if (line == NULL || len == 0u || line[0] != '#')
        return false;                 /* config buyrug'i emas */

    if (len > CONFIG_CMD_MAX)
    {
        LOG_XATO("CFG", "Buyruq juda uzun (%u belgi)", (unsigned)len);
        ConfigBlink(5u);
        return true;                  /* '#' bilan boshlangan - ID emas */
    }

    /* Chaqiruvchidagi satr yakunlanganiga tayanmaymiz va uni o'zgartirmaymiz */
    memcpy(cmd, line, len);
    cmd[len] = '\0';

    if (strncmp(cmd, "#SRV=", 5u) == 0)
    {
        applied = ApplyServer(cmd + 5u);
    }
    else if (strcmp(cmd, "#DBG=1") == 0)
    {
        s_cfg.debug = 1u;
        applied = true;
    }
    else if (strcmp(cmd, "#DBG=0") == 0)
    {
        s_cfg.debug = 0u;
        applied = true;
    }
    else if (strcmp(cmd, "#RESET") == 0)
    {
        LoadDefaults();
        applied = true;
    }

    if (!applied)
    {
        LOG_XATO("CFG", "Buyruq tanilmadi: \"%s\"", cmd);
        ConfigBlink(5u);
        return true;
    }

    /* Log ni yangi holatga o'tkazishdan OLDIN natijani chiqaramiz va
       buferni bo'shatamiz, aks holda #DBG=0 da foydalanuvchi tasdiqni
       ko'rmay qolardi */
    LOG_OK("CFG", "\"%s\" bajarildi | Server: %s:%u | Debug: %s",
           cmd, s_cfg.ip, (unsigned)s_cfg.port, s_cfg.debug ? "YOQIQ" : "O'CHIQ");
    Log_Flush(200u);

    /* Flash ga yozilmasa ham RAM dagi holat allaqachon o'zgargan, shuning
       uchun log shunga moslanadi; LED esa saqlanmaganini bildiradi. */
    Log_SetEnabled(s_cfg.debug != 0u);
    ConfigBlink(SaveToFlash() ? 2u : 5u);

    return true;
}

/* ================= Getterlar ================= */

const char *Config_GetServerIp(void)
{
    return s_cfg.ip;
}

uint16_t Config_GetServerPort(void)
{
    return s_cfg.port;
}

/*
 * Resolved server IP address (uint32_t), yoki 0 agar:
 *   - hozir IP address bo'lsa DNS kerak emas (IP address avval resolve qilingan)
 *   - domen resolve qilinayotgan bo'lsa (DNS javob kutilmoqda)
 *   - DNS timeout bo'lsa
 *
 * Proccess() ichida tekshiriladi: agar 0 bo'lsa IP yana DNS resolve qilinsa
 * callback natijasi yoziladi, aks holda TCP to'xtaydi.
 */
uint32_t Config_GetResolvedIp(void)
{
    /* DNS timeout tekshiruvi: 3s dan ko'p o'tgan bo'lsa fallback */
    if (s_cfg.resolved_ip == 0u &&
        s_cfg.ip[0] != '\0' &&
        !IsIpAddress(s_cfg.ip) &&
        (HAL_GetTick() - dns_timeout_start > DNS_TIMEOUT_MS))
    {
        LOG_XATO("DNS", "Timeout %ums - %s", DNS_TIMEOUT_MS, s_cfg.ip);
        s_cfg.resolved_ip = 0xFFFFFFFFu;  /* special: timeout belgisi */
    }

    return s_cfg.resolved_ip;
}

bool Config_IsDebugEnabled(void)
{
    return (s_cfg.debug != 0u);
}
