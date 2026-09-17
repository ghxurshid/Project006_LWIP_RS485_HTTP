/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * File Name          : LWIP.c
  * Description        : This file provides initialization code for LWIP
  *                      middleWare.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#include "aes.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/etharp.h"

#include <stdio.h>
#include <string.h>

#include "Queue.h"
#include "hid_reader.h"
#include "usb_host.h"

#include "log.h"
#include "config.h"

/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "lwip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#if defined ( __CC_ARM )  /* MDK ARM Compiler */
#include "lwip/sio.h"
#endif /* MDK ARM Compiler */
#include "ethernetif.h"

/* USER CODE BEGIN 0 */

/* Server manzili endi config.h dagi zavod qiymatlaridan boshlanadi va QR
   buyrug'i (#SRV=...) bilan o'zgartiriladi - Config_GetServerIp/Port() ga
   qarang. Shuning uchun bu yerda qattiq yozilgan manzil qolmadi. */

#define PHY_ADDRESS 0x01
#define TCP_CONNECT_TIMEOUT_MS  5000

#define AES_KEY_SIZE   16
#define AES_BLOCK_SIZE 16

static uint8_t aes_key[AES_KEY_SIZE] = {
	0x6E, 0x6C, 0x41, 0x74,
	0x72, 0x61, 0x69, 0x63,
	0x41, 0x74, 0x65, 0x6E,
	0x73, 0x6F, 0x4E, 0x65
};

extern Queue queue;

// TCP yuborish holati
typedef enum {
    TCP_SEND_IDLE,
    TCP_SEND_CONNECTING,
    TCP_SEND_CONNECTED,
    TCP_SEND_SENT,
    TCP_SEND_DONE,
    TCP_SEND_ERROR,
} TcpSendState;

static volatile TcpSendState tcp_state = TCP_SEND_IDLE;
static volatile bool tcp_pcb_freed = false;  // error_cb PCB ni free qilganini bildiradi

/* USER CODE END 0 */
/* Private function prototypes -----------------------------------------------*/
static void ethernet_link_status_updated(struct netif *netif);
static void Ethernet_Link_Periodic_Handle(struct netif *netif);
/* ETH Variables initialization ----------------------------------------------*/
void Error_Handler(void);

/* DHCP Variables initialization ---------------------------------------------*/
uint32_t DHCPfineTimer = 0;
uint32_t DHCPcoarseTimer = 0;
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
uint32_t EthernetLinkTimer;

/* Variables Initialization */
struct netif gnetif;
ip4_addr_t ipaddr;
ip4_addr_t netmask;
ip4_addr_t gw;

/* USER CODE BEGIN 2 */

// ============== AES ==============

static void AES_EncryptBuffer(uint8_t* buff, size_t length)
{
	struct AES_ctx ctx;
	AES_init_ctx(&ctx, aes_key);

	for (size_t i = 0; i < length; i += AES_BLOCKLEN)
	{
		AES_ECB_encrypt(&ctx, buff + i);
	}
}

// ============== TCP Callbacks ==============

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
	(void)arg;
	(void)tpcb;
	if (err == ERR_OK)
		tcp_state = TCP_SEND_CONNECTED;
	else
		tcp_state = TCP_SEND_ERROR;
	return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
	(void)arg;
	(void)tpcb;
	(void)len;
	tcp_state = TCP_SEND_DONE;
	return ERR_OK;
}

static void tcp_error_cb(void *arg, err_t err)
{
	(void)arg;
	tcp_pcb_freed = true;

	if (err == ERR_CLSD || err == ERR_ABRT) {
		// Server ulanishni yopdi yoki abort qildi — yuborish tugagandan keyin normal
		return;
	}

	LOG_XATO("TCP", "Callback xato: err=%d", err);
	tcp_state = TCP_SEND_ERROR;
}

// ============== TCP Send ==============

// Server javobini kutish paytidagi fon ishlari. Server o'chiq bo'lsa har urinish
// 5s gacha bloklaydi - USB host shu vaqtda aylantirilmasa QR skaner
// enumeratsiyasi va skanerlash server javobini kutib qotib qolardi.
static void TcpWait_Poll(void)
{
    MX_LWIP_Process();
    MX_USB_HOST_Process();
    HidReader_Process();
}

// PCB ni xavfsiz yopish — error_cb da free bo'lgan bo'lsa tegmaymiz
static void tcp_safe_close(struct tcp_pcb *pcb)
{
    if (tcp_pcb_freed)
        return;  // LWIP allaqachon free qilgan

    if (tcp_close(pcb) != ERR_OK)
        tcp_abort(pcb);  // close xato bersa abort qilamiz
}

static err_t SendDataRawTCP(const char *server_ip, uint16_t server_port,
                            uint64_t value)
{
    struct tcp_pcb *pcb;
    ip_addr_t server_addr;
    err_t err;

    LOG_INFO("TCP", "Yuborish: %lu -> %s:%u", (unsigned long)value, server_ip, server_port);

    // ID ni ASCII stringga aylantirish
    char str[21];
    int len = snprintf(str, sizeof(str), "%lu", (unsigned long)value);

    // PKCS7 padding bilan AES blokga joylashtirish
    uint8_t padded_message[AES_BLOCKLEN];
    memcpy(padded_message, str, len);
    uint8_t pad = AES_BLOCKLEN - len;
    memset(padded_message + len, pad, pad);
    AES_EncryptBuffer(padded_message, AES_BLOCKLEN);

    // Socket yaratish
    pcb = tcp_new();
    if (pcb == NULL)
    {
        LOG_XATO("TCP", "Socket yaratib bo'lmadi (xotira yetishmadi)");
        return ERR_MEM;
    }

    tcp_pcb_freed = false;
    tcp_err(pcb, tcp_error_cb);
    tcp_sent(pcb, tcp_sent_cb);
    ipaddr_aton(server_ip, &server_addr);

    // Ulanish boshlash
    tcp_state = TCP_SEND_CONNECTING;
    err = tcp_connect(pcb, &server_addr, server_port, tcp_connected_cb);
    if (err != ERR_OK)
    {
        LOG_XATO("TCP", "Ulanish boshlanmadi: err=%d", err);
        tcp_safe_close(pcb);
        tcp_state = TCP_SEND_IDLE;
        return err;
    }

    // Ulanish kutish
    uint32_t start = sys_now();
    while (tcp_state == TCP_SEND_CONNECTING)
    {
        TcpWait_Poll();
        if (sys_now() - start > TCP_CONNECT_TIMEOUT_MS)
        {
            LOG_XATO("TCP", "Ulanish timeout: %ums", TCP_CONNECT_TIMEOUT_MS);
            if (!tcp_pcb_freed) tcp_abort(pcb);
            tcp_state = TCP_SEND_IDLE;
            return ERR_TIMEOUT;
        }
    }

    if (tcp_state == TCP_SEND_ERROR)
    {
        LOG_XATO("TCP", "Server rad etdi");
        tcp_state = TCP_SEND_IDLE;
        return ERR_CONN;
    }

    LOG_INFO("TCP", "Ulandi, ma'lumot yuborilmoqda...");

    // Ma'lumot yozish
    err = tcp_write(pcb, padded_message, AES_BLOCKLEN, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        LOG_XATO("TCP", "Yozish xatosi: err=%d", err);
        tcp_safe_close(pcb);
        tcp_state = TCP_SEND_IDLE;
        return err;
    }
    tcp_output(pcb);

    // ACK kutish
    tcp_state = TCP_SEND_SENT;
    start = sys_now();
    while (tcp_state == TCP_SEND_SENT)
    {
        TcpWait_Poll();
        if (sys_now() - start > TCP_CONNECT_TIMEOUT_MS)
        {
            LOG_XATO("TCP", "ACK kutish timeout: %ums", TCP_CONNECT_TIMEOUT_MS);
            if (!tcp_pcb_freed) tcp_abort(pcb);
            tcp_state = TCP_SEND_IDLE;
            return ERR_TIMEOUT;
        }
    }

    if (tcp_state == TCP_SEND_DONE)
    {
        tcp_safe_close(pcb);
        uint32_t elapsed = sys_now() - start;
        LOG_OK("TCP", "Yuborildi: %lu (%lums)", (unsigned long)value, (unsigned long)elapsed);
        tcp_state = TCP_SEND_IDLE;
        return ERR_OK;
    }

    LOG_XATO("TCP", "Yuborish muvaffaqiyatsiz");
    tcp_state = TCP_SEND_IDLE;
    return ERR_CONN;
}

// ============== LED ==============

static void SetLEDState(uint8_t state)
{
    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, (state & 0x1) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, (state & 0x2) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, (state & 0x4) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void SuccessLED(void)
{
	const uint8_t seq[] = {0b000, 0b100, 0b110, 0b011, 0b001, 0b000};
	for (size_t i = 0; i < sizeof(seq); i++)
	{
		SetLEDState(seq[i]);
		HAL_Delay(80);
	}
}

static void FailLED(void)
{
	const uint8_t seq[] = {0b101, 0b011, 0b110, 0b111, 0b000};
	for (size_t i = 0; i < sizeof(seq); i++)
	{
		SetLEDState(seq[i]);
		HAL_Delay(80);
	}
}

// ============== Main Process ==============

static const char *DataTypeName(DataType type)
{
    switch (type)
    {
    case RS485_TYPE:   return "RS485";
    case HID_TYPE:     return "HID";
    default:           return "?";
    }
}

void Proccess(void)
{
    // Queue dan serverga yuborish (faqat IP olingan va DNS resolved bo'lsa)
    QueueItem item;
    if (gnetif.ip_addr.addr != 0 && Queue_Peek(&queue, &item))
    {
        const char *src = DataTypeName(item.dataType);
        uint32_t resolved = Config_GetResolvedIp();

        /* Manzil hali tayyor emas: DNS so'ralmoqda yoki javob bermadi.
           Element queue da QOLADI - kartani qayta bosdirmaslik uchun.
           Bu yerda log yozilmaydi: main loop juda tez aylanadi va har
           aylanishda satr yozilsa log buferi bir zumda to'lib ketardi.
           Sababini Config_GetResolvedIp() o'zi bir marta yozadi. */
        if (resolved == 0u || resolved == CONFIG_IP_UNRESOLVED)
            return;

        /* Resolved IP bilan TCP ulanish. uint32_t dan string'ga aylantiramiz. */
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                 (unsigned)((resolved >>  0) & 0xFFu),
                 (unsigned)((resolved >>  8) & 0xFFu),
                 (unsigned)((resolved >> 16) & 0xFFu),
                 (unsigned)((resolved >> 24) & 0xFFu));

        if (SendDataRawTCP(ip_str, Config_GetServerPort(), item.value) == ERR_OK)
        {
            Queue_Dequeue(&queue, NULL);
            SuccessLED();
            LOG_OK("SEND", "[%s] Yuborildi: %lu | queue: %u/%u", src, (unsigned long)item.value, Queue_Count(&queue), (unsigned)QUEUE_MAX_ITEMS);
        }
        else
        {
            FailLED();
            LOG_XATO("SEND", "[%s] Yuborilmadi: %lu | queue: %u/%u", src, (unsigned long)item.value, Queue_Count(&queue), (unsigned)QUEUE_MAX_ITEMS);
        }
    }
}

void netif_status_callback(struct netif *netif)
{
	if (netif_is_up(netif) && netif->ip_addr.addr != 0) {
		LOG_OK("NET", "DHCP IP olindi: %s", ipaddr_ntoa(&netif->ip_addr));
	} else {
		LOG_INFO("NET", "DHCP kutilmoqda...");
	}
}
/* USER CODE END 2 */

/**
  * LwIP initialization function
  */
void MX_LWIP_Init(void)
{
  /* Initilialize the LwIP stack without RTOS */
  lwip_init();

  /* IP addresses initialization with DHCP (IPv4) */
  ipaddr.addr = 0;
  netmask.addr = 0;
  gw.addr = 0;

  /* add the network interface (IPv4/IPv6) without RTOS */
  netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &ethernet_input);

  /* Registers the default network interface */
  netif_set_default(&gnetif);

  if (netif_is_link_up(&gnetif))
  {
    /* When the netif is fully configured this function must be called */
    netif_set_up(&gnetif);
  }
  else
  {
    /* When the netif link is down this function must be called */
    netif_set_down(&gnetif);
  }

  /* Set the link callback function, this function is called on change of link status*/
  netif_set_link_callback(&gnetif, ethernet_link_status_updated);

  /* Create the Ethernet link handler thread */

  /* Start DHCP negotiation for a network interface (IPv4) */
  dhcp_start(&gnetif);

/* USER CODE BEGIN 3 */
  netif_set_status_callback(&gnetif, netif_status_callback);

  /* DHCP yuqorida, generatsiya qilingan kodda allaqachon boshlangan -
     bu yerda qayta chaqirilsa ikkinchi DISCOVER ketadi. Faqat holatni
     bildiramiz; kabel keyin ulansa ethernet_link_status_updated() ni
     qayta boshlaydi. */
  if (netif_is_link_up(&gnetif))
  {
      LOG_OK("NET", "Ethernet allaqachon ulangan");
      LOG_INFO("NET", "DHCP boshlandi");
  }
  else
  {
      LOG_INFO("NET", "Ethernet kabel kutilmoqda...");
  }
/* USER CODE END 3 */
}

#ifdef USE_OBSOLETE_USER_CODE_SECTION_4
/* Kept to help code migration. (See new 4_1, 4_2... sections) */
/* Avoid to use this user section which will become obsolete. */
/* USER CODE BEGIN 4 */
/* USER CODE END 4 */
#endif

/**
  * @brief  Ethernet Link periodic check
  * @param  netif
  * @retval None
  */
static void Ethernet_Link_Periodic_Handle(struct netif *netif)
{
/* USER CODE BEGIN 4_4_1 */
/* USER CODE END 4_4_1 */

  /* Ethernet Link every 100ms */
  if (HAL_GetTick() - EthernetLinkTimer >= 100)
  {
    EthernetLinkTimer = HAL_GetTick();
    ethernet_link_check_state(netif);
  }
/* USER CODE BEGIN 4_4 */
/* USER CODE END 4_4 */
}

/**
 * ----------------------------------------------------------------------
 * Function given to help user to continue LwIP Initialization
 * Up to user to complete or change this function ...
 * Up to user to call this function in main.c in while (1) of main(void)
 *-----------------------------------------------------------------------
 * Read a received packet from the Ethernet buffers
 * Send it to the lwIP stack for handling
 * Handle timeouts if LWIP_TIMERS is set and without RTOS
 * Handle the llink status if LWIP_NETIF_LINK_CALLBACK is set and without RTOS
 */
void MX_LWIP_Process(void)
{
/* USER CODE BEGIN 4_1 */
/* USER CODE END 4_1 */
  ethernetif_input(&gnetif);

/* USER CODE BEGIN 4_2 */
/* USER CODE END 4_2 */
  /* Handle timeouts */
  sys_check_timeouts();

  Ethernet_Link_Periodic_Handle(&gnetif);

/* USER CODE BEGIN 4_3 */
/* USER CODE END 4_3 */
}

/**
  * @brief  Notify the User about the network interface config status
  * @param  netif: the network interface
  * @retval None
  */
static void ethernet_link_status_updated(struct netif *netif)
{
  if (netif_is_up(netif))
  {
/* USER CODE BEGIN 5 */
	LOG_OK("NET", "Ethernet ulandi (link UP)");
	LOG_INFO("NET", "DHCP boshlanmoqda...");
	dhcp_start(&gnetif);
/* USER CODE END 5 */
  }
  else /* netif is down */
  {
/* USER CODE BEGIN 6 */
	  LOG_XATO("NET", "Ethernet uzildi (link DOWN)");
/* USER CODE END 6 */
  }
}

#if defined ( __CC_ARM )  /* MDK ARM Compiler */
/**
 * Opens a serial device for communication.
 *
 * @param devnum device number
 * @return handle to serial device if successful, NULL otherwise
 */
sio_fd_t sio_open(u8_t devnum)
{
  sio_fd_t sd;

/* USER CODE BEGIN 7 */
  sd = 0;
/* USER CODE END 7 */

  return sd;
}

/**
 * Sends a single character to the serial device.
 *
 * @param c character to send
 * @param fd serial device handle
 *
 * @note This function will block until the character can be sent.
 */
void sio_send(u8_t c, sio_fd_t fd)
{
/* USER CODE BEGIN 8 */
/* USER CODE END 8 */
}

/**
 * Reads from the serial device.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received - may be 0 if aborted by sio_read_abort
 *
 * @note This function will block until data can be received. The blocking
 * can be cancelled by calling sio_read_abort().
 */
u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 9 */
  recved_bytes = 0;
/* USER CODE END 9 */
  return recved_bytes;
}

/**
 * Tries to read from the serial device. Same as sio_read but returns
 * immediately if no data is available and never blocks.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received
 */
u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 10 */
  recved_bytes = 0;
/* USER CODE END 10 */
  return recved_bytes;
}
#endif /* MDK ARM Compiler */

