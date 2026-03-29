#include "icmp.h"

// Расчёт контрольной суммы 
USHORT calculate_checksum(USHORT* buffer, ULONG size) {
    ULONG sum = 0;

    while (size > 1) {
        sum += *buffer++;
        size -= 2;
    }

    // Если остался нечётный байт
    if (size) {
        sum += *reinterpret_cast<uint8_t*>(buffer);
    }

    // Сворачиваем переносы до 16 бит
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~static_cast<USHORT>(sum);
}