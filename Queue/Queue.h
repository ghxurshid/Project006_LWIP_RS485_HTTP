/*
 * Queue.h - Static Ring Buffer (interrupt-safe, lock-free)
 *
 *  Created on: Mar 7, 2025
 *      Author: Xurshid Xujamatov
 */

#ifndef QUEUE_H_
#define QUEUE_H_

#include "stdbool.h"
#include "stdint.h"
#include "stddef.h"

#define QUEUE_MAX_ITEMS 5800

typedef uint8_t DataType;
#define RS485_TYPE   0
#define WIEGAND_TYPE 1

typedef struct __attribute__((packed)) {
    uint64_t value;
    DataType dataType;
} QueueItem;  // 9 bayt (packed)

typedef struct {
    QueueItem items[QUEUE_MAX_ITEMS];
    volatile uint16_t head;
    volatile uint16_t tail;
} Queue;

void     Queue_Init(Queue* q);
bool     Queue_IsEmpty(Queue* q);
bool     Queue_IsFull(Queue* q);
bool     Queue_Enqueue(Queue* q, DataType type, uint64_t value);
bool     Queue_Dequeue(Queue* q, QueueItem* out);
bool     Queue_Peek(Queue* q, QueueItem* out);
uint16_t Queue_Count(Queue* q);

#endif /* QUEUE_H_ */
