/*
 * Queue.c - Static Ring Buffer (interrupt-safe)
 *
 * Multi-producer / single-consumer model:
 *   - Producerlar faqat head ni yozadi: RS485 (USART2 ISR), Wiegand va
 *     USB HID (main loop)
 *   - Consumer (main loop, Proccess) faqat tail ni yozadi
 *   - Enqueue qisqa critical section ichida: aks holda main loop dagi
 *     enqueue o'rtasida ISR enqueue qilsa ikkalasi bitta katakka yozib,
 *     bittasi jimgina yo'qolardi
 *   - Dequeue/Peek ga lock kerak emas: tail ni faqat consumer yozadi
 *
 *  Created on: Mar 7, 2025
 *      Author: Xurshid Xujamatov
 */

#include "Queue.h"
#include "stm32f4xx.h"  /* __get_PRIMASK / __disable_irq */

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
    /* PRIMASK saqlanadi: ISR ichidan chaqirilganda ham uzilishlarni noto'g'ri yoqib yubormaydi */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    bool ok = !Queue_IsFull(q);
    if (ok)
    {
        QueueItem* item = &q->items[q->head];
        item->dataType = type;
        item->value    = value;

        q->head = (q->head + 1) % QUEUE_MAX_ITEMS;
    }

    __set_PRIMASK(primask);
    return ok;
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
