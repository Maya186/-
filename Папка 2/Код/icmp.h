#pragma once
#include <winsock2.h>
#include <ws2ipdef.h>
#include <cstdint>

// ICMP заголовок (8 байт)
#pragma pack(push, 1)
struct ICMPHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
};

// IP заголовок (для парсинга ответов)
struct IPHeader {
    uint8_t  ihl : 4, version : 4;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t ident;
    uint16_t frags;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
};
#pragma pack(pop)

// Константы типов ICMP
#define ICMP_ECHO_REQUEST   8
#define ICMP_ECHO_REPLY     0
#define ICMP_TIME_EXCEEDED  11

// Функция расчёта контрольной суммы
USHORT calculate_checksum(USHORT* buffer, ULONG size);