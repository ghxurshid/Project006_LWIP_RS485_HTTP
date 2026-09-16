/*
 * config.h - QR skaner orqali o'rnatiladigan ish sozlamalari
 *
 * Qurilma obyektda turganda unga COM kabel ulash har doim ham mumkin emas,
 * shuning uchun server manzili va debug chiqishi maxsus QR kodlarni skanerlash
 * bilan o'zgartiriladi:
 *
 *   QR -> HidReader -> FinishScan() -> Config_TryCommand()
 *                                   -> RAM da yangilanadi + flash ga saqlanadi
 *                                   -> LED3 bilan tasdiq
 *
 * Config QR lari queue ga TUSHMAYDI - ular ID emas, buyruq.
 *
 * Nega '#' prefiksi: ParseValue() faqat raqamlardan iborat kodni qabul qiladi,
 * shuning uchun '#' bilan boshlangan satr oddiy ID oqimi bilan hech qachon
 * to'qnashmaydi - qo'shimcha "konfiguratsiya rejimi" kerak emas.
 *
 * DIQQAT: hozircha config QR lari himoyalanmagan - kim shunday QR chop etsa,
 * qurilmani istalgan serverga yo'naltira oladi. Obyektga o'rnatishdan oldin
 * buyruqqa sirli prefiks qo'shish yoki jumper bilan cheklash kerak bo'ladi.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

/* Ishchi (zavod) server - #SRV=PROD shunga qaytaradi */
#define CONFIG_PROD_SERVER_IP   "185.74.5.250"
#define CONFIG_PROD_SERVER_PORT 5488

/* Sinov serveri - #SRV=TEST shunga o'tkazadi */
#define CONFIG_TEST_SERVER_IP   "10.0.40.18"
#define CONFIG_TEST_SERVER_PORT 23

/* Zavod holatida debug chiqishi yoqilgan */
#define CONFIG_DEFAULT_DEBUG    true

/* "255.255.255.255" + '\0' */
#define CONFIG_IP_STR_MAX       16u

/*
 * Flash dan sozlamalarni o'qiydi. Yozuv yo'q yoki buzilgan bo'lsa zavod
 * qiymatlari ishlatiladi. Log ishga tushgandan keyin, main loop dan oldin
 * bir marta chaqiriladi.
 */
void Config_Init(void);

/*
 * Skanerlangan satr config buyrug'imi?
 *
 * true qaytsa - satr buyruq sifatida ishlangan (muvaffaqiyatli yoki xato
 * bo'lishidan qat'i nazar) va chaqiruvchi uni ID deb qayta ishlamasligi,
 * queue ga qo'ymasligi kerak.
 * false qaytsa - bu config buyrug'i emas, odatdagi yo'l bilan davom etiladi.
 *
 * Qo'llab-quvvatlanadigan buyruqlar:
 *   #SRV=<ip>:<port>   masalan #SRV=192.168.1.50:5488
 *   #SRV=PROD          zavod serveriga qaytaradi
 *   #SRV=TEST          sinov serveriga o'tkazadi
 *   #DBG=1 / #DBG=0    UART log chiqishini yoqadi / o'chiradi
 *   #RESET             hamma sozlamani zavod holatiga qaytaradi
 */
bool Config_TryCommand(const char *line, uint8_t len);

/* Joriy server manzili - Proccess() har yuborishdan oldin shundan oladi */
const char *Config_GetServerIp(void);
uint16_t    Config_GetServerPort(void);

/* Debug chiqishi holati (ma'lumot uchun; log ni Log_SetEnabled() boshqaradi) */
bool Config_IsDebugEnabled(void);

#endif /* CONFIG_H_ */
