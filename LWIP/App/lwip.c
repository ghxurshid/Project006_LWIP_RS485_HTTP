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
#include "lwip/api.h"
#include "stdbool.h"
#include "lwip/sockets.h"
#include <stdatomic.h>

/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "lwip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#if defined ( __CC_ARM )  /* MDK ARM Compiler */
#include "lwip/sio.h"
#endif /* MDK ARM Compiler */
#include "ethernetif.h"
#include <string.h>

/* USER CODE BEGIN 0 */

// Параметры сервера
#define SERVER_IP "185.74.5.250"   // IP-адрес сервера
#define SERVER_PORT 5488           // Порт сервера
#define SERVER_IP_TEST "10.0.40.212"   // IP-адрес сервера
#define SERVER_PORT_TEST 23           // Порт сервера

#define PHY_ADDRESS 0x01  // Адрес вашего PHY (уточните по схеме подключения)

#define AES_KEY_SIZE 16
#define AES_BLOCK_SIZE 16

atomic_int IsLinkUp = 0;

extern osMessageQueueId_t mainQueueHandle;

uint8_t key[AES_KEY_SIZE] = {
  0x6E, 0x6C, 0x41, 0x74,
  0x72, 0x61, 0x69, 0x63,
  0x41, 0x74, 0x65, 0x6E,
  0x73, 0x6F, 0x4E, 0x65
};

/* USER CODE END 0 */
/* Private function prototypes -----------------------------------------------*/
static void ethernet_link_status_updated(struct netif *netif);
/* ETH Variables initialization ----------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* Variables Initialization */
struct netif gnetif;
ip4_addr_t ipaddr;
ip4_addr_t netmask;
ip4_addr_t gw;
/* USER CODE BEGIN OS_THREAD_ATTR_CMSIS_RTOS_V2 */
#define INTERFACE_THREAD_STACK_SIZE ( 2084 )
osThreadAttr_t attributes;
/* USER CODE END OS_THREAD_ATTR_CMSIS_RTOS_V2 */

/* USER CODE BEGIN 2 */
void MonitorAllTasks() {
	TaskStatus_t *taskStatusArray = pvPortMalloc(10 * sizeof(TaskStatus_t));
	if (taskStatusArray == NULL) {
		printf("Failed to allocate memory for task monitoring\n");
		return;
	}

	// Получить информацию обо всех задачах
	UBaseType_t taskCount = uxTaskGetSystemState(taskStatusArray, 10, NULL);

	printf("- - - - - - - - - - - - \n");

	for (UBaseType_t i = 0; i < taskCount; i++) {
		// Полный размер стека задачи
		//UBaseType_t totalStackSize = taskStatusArray[i].usStackHighWaterMark;

		// Минимальный остаток стека (HighWaterMark)
		UBaseType_t remainingStack = taskStatusArray[i].usStackHighWaterMark;

		// Расчёт использованного стека
		UBaseType_t usedStack = remainingStack;

		// Расчёт процента использованного стека
		//float usagePercent = ((float)usedStack / (float)totalStackSize) * 100.0f;

		// Вывод информации
		printf("Task: %s, Used: %lu words\n",
			   taskStatusArray[i].pcTaskName,
			   usedStack);
	}

	printf("- - - - - - - - - - - - \n");
}

void AES_Encrypt(char* buff, size_t length)
{
	struct AES_ctx ctx;

	// �?нициализация контекста AES с заданным ключом
	AES_init_ctx(&ctx, key);

	// Шифрование данных (в данном случае ECB режим)
	for (int i = 0 ; i < length; i += AES_BLOCKLEN)
	{
		AES_ECB_encrypt(&ctx, (buff + i));
	}
}

void AES_Decrypt(char* buff, size_t length)
{
    struct AES_ctx ctx;

    // �?нициализация контекста AES с заданным ключом
    AES_init_ctx(&ctx, key);

    // Дешифрование данных
    for (int i = 0 ; i < length; i += AES_BLOCKLEN)
	{
    	AES_ECB_decrypt(&ctx, buff);
		buff += AES_BLOCKLEN;
	}
}

int SendDataOverSocket(const char *server_ip, uint16_t server_port, char *message)
{
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = lwip_htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    osDelay(10);
    if (sock < 0) {
        printf("Error: Unable to create socket\n");
        return sock;
    }

    int flags = lwip_fcntl(sock, F_GETFL, 0);
    lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = lwip_connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    osDelay(10);
    if (ret < 0 && errno != EINPROGRESS) {
        printf("Error: Connection failed, errno = %d\n", errno);
        lwip_close(sock);
        return sock;
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);

    ret = lwip_select(sock + 1, NULL, &writefds, NULL, &timeout);
    osDelay(10);
    if (ret <= 0) {
        printf("Error: select() timeout or failed, ret = %d, errno = %d\n", ret, errno);
        lwip_close(sock);
        return -1;
    }

    int so_error;
    socklen_t len = sizeof(so_error);
    lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);

    if (so_error != 0) {
        printf("Error: Connection error, so_error = %d\n", so_error);
        lwip_close(sock);
        return -1;
    }

    size_t length = strlen(message);
    size_t padded_length = ((length + AES_BLOCKLEN - 1) / AES_BLOCKLEN) * AES_BLOCKLEN;
    char *padded_message = malloc(padded_length);

    if (!padded_message) {
        printf("Error: Memory allocation failed\n");
        lwip_close(sock);
        return -1;
    }

    memset(padded_message, padded_length - length, padded_length);
    strncpy(padded_message, message, length);

    AES_Encrypt(padded_message, padded_length);

    ret = lwip_send(sock, padded_message, padded_length, 0);
    osDelay(10);
    if (ret < 0) {
        printf("Error: Failed to send data, errno = %d\n", errno);
    } else {
        printf("Data sent successfully!\n");
    }

    free(padded_message);
    lwip_fcntl(sock, F_SETFL, flags);
    lwip_close(sock);

    return ret;
}


void SetLEDState(uint8_t state)
{
    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, (state & 0x1) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, (state & 0x2) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, (state & 0x4) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void SuccessLED() {
	const uint8_t led_sequence[] = {0b000, 0b100, 0b110, 0b011, 0b001, 0b000};
	const size_t sequence_length = sizeof(led_sequence) / sizeof(led_sequence[0]);

	for (size_t i = 0; i < sequence_length; i++)
	{
		SetLEDState(led_sequence[i]);
		osDelay(80);
	}
}

void FailLED() {
	const uint8_t led_sequence[] = {0b101, 0b011, 0b110, 0b111, 0b000};
	const size_t sequence_length = sizeof(led_sequence) / sizeof(led_sequence[0]);

	for (size_t i = 0; i < sequence_length; i++)
	{
		SetLEDState(led_sequence[i]);
		osDelay(80);
	}
}

void MX_LWIP_Proccess()
{
    while (1)
    {
        if (!netif_is_up(&gnetif))
        {
            osDelay(1000);
            continue;
        }

        char *message;
        osStatus_t status = osMessageQueueGet(mainQueueHandle, &message, NULL, osWaitForever);

        if (status == osOK)
        {
        	if (strcmp(message, "8883250") == 0) {
        		SendDataOverSocket(SERVER_IP_TEST, SERVER_PORT_TEST, message);
        	}
            int ret = SendDataOverSocket(SERVER_IP, SERVER_PORT, message);

            if (ret > 0) {
				free(message);
				SuccessLED();
			} else {
				osMessageQueuePut(mainQueueHandle, &message, 0, osWaitForever);
				FailLED();
			}
        }

        osDelay(1000);
    }
}

/* USER CODE END 2 */

/**
  * LwIP initialization function
  */
void MX_LWIP_Init(void)
{
  /* Initilialize the LwIP stack with RTOS */
  tcpip_init( NULL, NULL );

  /* IP addresses initialization with DHCP (IPv4) */
  ipaddr.addr = 0;
  netmask.addr = 0;
  gw.addr = 0;

  /* add the network interface (IPv4/IPv6) with RTOS */
  netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &tcpip_input);

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
/* USER CODE BEGIN H7_OS_THREAD_NEW_CMSIS_RTOS_V2 */
  memset(&attributes, 0x0, sizeof(osThreadAttr_t));
  attributes.name = "EthLink";
  attributes.stack_size = INTERFACE_THREAD_STACK_SIZE;
  attributes.priority = osPriorityBelowNormal;
  osThreadNew(ethernet_link_thread, &gnetif, &attributes);
/* USER CODE END H7_OS_THREAD_NEW_CMSIS_RTOS_V2 */

  /* Start DHCP negotiation for a network interface (IPv4) */
  dhcp_start(&gnetif);

/* USER CODE BEGIN 3 */
  while(gnetif.ip_addr.addr == 0);
  printf("IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
/* USER CODE END 3 */
}

#ifdef USE_OBSOLETE_USER_CODE_SECTION_4
/* Kept to help code migration. (See new 4_1, 4_2... sections) */
/* Avoid to use this user section which will become obsolete. */
/* USER CODE BEGIN 4 */
/* USER CODE END 4 */
#endif

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
	//printf("Link is UP. IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
/* USER CODE END 5 */
  }
  else /* netif is down */
  {
/* USER CODE BEGIN 6 */
	  //printf("Link is DOWN.\n");
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
  sd = 0; // dummy code
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
  recved_bytes = 0; // dummy code
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
  recved_bytes = 0; // dummy code
/* USER CODE END 10 */
  return recved_bytes;
}
#endif /* MDK ARM Compiler */

