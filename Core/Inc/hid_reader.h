/*
 * hid_reader.h - USB HID (klaviatura emulyatsiyasi) orqali QR skaner o'quvchi
 *
 * QR50BE kabi skanerlar USB ga "HID keyboard" rejimida ulanganda o'qilgan
 * kodni tugma bosishlar ketma-ketligi sifatida yuboradi, oxirida odatda Enter.
 *
 *   OTG_HS -> USBH_Process() -> usbh_hidkbd (report descriptor bo'yicha dekod)
 *          -> USBH_HIDKBD_ReportCallback() -> satr yig'iladi
 *          -> Enter/Tab yoki pauza -> raqamga aylantiriladi
 *          -> Queue_Enqueue(HID_TYPE)
 *
 * Keyin Proccess() uni Wiegand va RS485 bilan bir xil yo'l orqali serverga
 * yuboradi.
 *
 * Created on: 2026
 * Author: Xurshid Xujamatov
 */

#ifndef HID_READER_H_
#define HID_READER_H_

#include "Queue.h"

/*
 * Belgilar orasidagi eng uzun pauza. Skaner suffiks (Enter) yubormaydigan
 * qilib sozlangan bo'lsa ham satr shu vaqtdan keyin yakunlanadi.
 * Skaner har bir belgini bosish+qo'yish = 2 hisobot bilan yuboradi, host esa
 * hisobotlarni endpoint bInterval i bo'yicha (kamida 4ms) so'raydi, ya'ni
 * normal oqimda belgilar orasi 8..20ms.
 */
#define HID_READER_SCAN_TIMEOUT_MS 150u

/* Bitta skanerlashdagi maksimal belgilar soni (terminatorsiz). */
#define HID_READER_LINE_MAX        32u

/*
 * Qabul qilinadigan eng katta qiymat. SendDataRawTCP() qiymatni
 * "(unsigned long)" ga keltirib formatlaydi, Cortex-M da bu 32 bit - kattaroq
 * son serverga jimgina BOSHQA raqam bo'lib ketardi. Shuning uchun bunday kod
 * queue ga umuman qo'shilmaydi.
 */
#define HID_READER_MAX_VALUE       0xFFFFFFFFu

/* Main loop dan oldin, Queue_Init() dan keyin chaqiriladi. */
void HidReader_Init(Queue *q);

/*
 * Har main loop aylanishida, MX_USB_HOST_Process() dan keyin chaqiriladi.
 * Qurilma ulanish/uzilishini kuzatadi va suffikssiz skanerlashni pauza bo'yicha
 * yakunlaydi. Hisobotlarning o'zi USBH_HID_EventCallback() da qayta ishlanadi.
 */
void HidReader_Process(void);

#endif /* HID_READER_H_ */
