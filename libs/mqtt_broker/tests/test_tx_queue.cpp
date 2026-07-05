#include "mqtt_broker/tx_queue.hpp"

#include <cassert>

using namespace mqtt_broker;

static void test_logical_budget()
{
    TxQueue q({4, 100});
    PendingDelivery pd = {};
    assert(q.enqueue(pd, 40));
    assert(q.enqueue(pd, 40));
    assert(!q.enqueue(pd, 30));
    assert(q.logical_bytes() == 80);

    uint32_t released = 0;
    assert(q.pop(&released));
    assert(released == 40);
    assert(q.logical_bytes() == 40);
}

int main()
{
    test_logical_budget();
    return 0;
}