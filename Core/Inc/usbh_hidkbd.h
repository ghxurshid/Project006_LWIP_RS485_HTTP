/*
 * usbh_hidkbd.h - USB Host: istalgan HID klaviatura uchun class drayver
 *
 * ST ning USBH_HID_CLASS i faqat bitta interfeysli "boot" klaviaturani
 * taniydi (SubClass=01, Protocol=01) va barcha so'rovlarni 0-interfeysga
 * yuboradi. QR50BE kabi composite skanerlarda klaviatura 1-interfeysda
 * turadi va 03/00/00 bilan e'lon qilinadi - ST drayveri uni rad etadi.
 *
 * Bu drayver:
 *   - har bir HID interfeysning report descriptor ini to'g'ri wIndex bilan
 *     o'qib, Keyboard/Keypad (Generic Desktop 0x06/0x07) collection ni topadi
 *   - report formatini (Report ID, modifier va tugma maydonlari) descriptor
 *     dan oladi; boot klaviaturada esa boot protokolni yoqadi
 *   - faqat shu interfeysning interrupt IN endpoint ini so'raydi
 *   - har hisobotni USBH_HIDKBD_Report ga keltirib ilovaga beradi
 *
 * Heap ishlatmaydi (bitta qurilma, statik holat). CubeMX dan tashqarida
 * turadi, ST HID class o'rniga USBH_HIDKBD_Install() bilan ulanadi.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#ifndef USBH_HIDKBD_H_
#define USBH_HIDKBD_H_

#include "usbh_core.h"

/* USBH_HIDKBD_Report.modifiers bitlari (Keyboard usage 0xE0..0xE7) */
#define HIDKBD_MOD_LCTRL   0x01u
#define HIDKBD_MOD_LSHIFT  0x02u
#define HIDKBD_MOD_LALT    0x04u
#define HIDKBD_MOD_LGUI    0x08u
#define HIDKBD_MOD_RCTRL   0x10u
#define HIDKBD_MOD_RSHIFT  0x20u
#define HIDKBD_MOD_RALT    0x40u
#define HIDKBD_MOD_RGUI    0x80u

#define HIDKBD_MAX_KEYS    6u

/* Qurilma formatidan qat'i nazar bir xil ko'rinishdagi klaviatura hisoboti */
typedef struct {
    uint8_t modifiers;               /* HIDKBD_MOD_* */
    uint8_t keys[HIDKBD_MAX_KEYS];   /* bosilgan tugmalar (Keyboard usage page), 0 = bo'sh */
} USBH_HIDKBD_Report;

/*
 * ST HID class ni host dagi ro'yxatda shu drayver bilan almashtiradi (HID
 * class ro'yxatda bo'lmasa, qo'shadi). MX_USB_HOST_Init() ichida,
 * USBH_RegisterClass() dan keyin chaqiriladi.
 */
USBH_StatusTypeDef USBH_HIDKBD_Install(USBH_HandleTypeDef *phost);

/*
 * Har bir qabul qilingan klaviatura hisobotida chaqiriladi. Ilova amalga
 * oshirishi SHART (weak emas - unutilsa link xatosi beradi, jim qolmaydi).
 * USBH_Process() ichidan, ya'ni main loop kontekstida chaqiriladi.
 */
void USBH_HIDKBD_ReportCallback(USBH_HandleTypeDef *phost, const USBH_HIDKBD_Report *report);

#endif /* USBH_HIDKBD_H_ */
