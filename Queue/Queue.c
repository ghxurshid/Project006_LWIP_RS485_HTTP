/*
 * Queue.c - Static Ring Buffer (interrupt-safe, lock-free)
 *
 * Single-producer / single-consumer model:
 *   - Interrupt (producer) faqat head ni yozadi
 *   - Main loop (consumer) faqat tail ni yozadi
 *   - Shu sababli mutex/disable_irq kerak emas
 *
 *  Created on: Mar 7, 2025
 *      Author: Xurshid Xujamatov
 */

#include "Queue.h"

void Queue_Init(Queue* q)
{
    q->head = 0;
    q->tail = 0;
}

bool Queue_IsEmpty(Queue* q)
{
    return (q->head == q->tail);
}

bool Queue_IsFull(Queue* q)
{
    uint16_t next = (q->head + 1) % QUEUE_MAX_ITEMS;
    return (next == q->tail);
}

uint16_t Queue_Count(Queue* q)
{
    return (q->head - q->tail + QUEUE_MAX_ITEMS) % QUEUE_MAX_ITEMS;
}

bool Queue_Enqueue(Queue* q, DataType type, uint64_t value)
{
    if (Queue_IsFull(q))
        return false;

    QueueItem* item = &q->items[q->head];
    item->dataType = type;
    item->value    = value;

    q->head = (q->head + 1) % QUEUE_MAX_ITEMS;
    return true;
}

bool Queue_Peek(Queue* q, QueueItem* out)
{
    if (Queue_IsEmpty(q))
        return false;

    *out = q->items[q->tail];
    return true;
}

bool Queue_Dequeue(Queue* q, QueueItem* out)
{
    if (Queue_IsEmpty(q))
        return false;

    if (out != NULL)
        *out = q->items[q->tail];

    q->tail = (q->tail + 1) % QUEUE_MAX_ITEMS;
    return true;
}
