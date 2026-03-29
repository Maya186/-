#include "utils.h"
#include <ws2tcpip.h>
#include <cstdio>

// Парсинг IP-адреса 
// Возвращает IP в сетевом порядке байт, или INADDR_NONE при ошибке
UINT32 parse_ip_address(const char* ip_str) {
    return inet_addr(ip_str);  
}

// Преобразование IP (в сетевом порядке) в строку вида "192.168.1.1"
std::string ip_to_string(UINT32 ip) {
    char buffer[16];
    UINT32 ip_host = ntohl(ip);  // Конвертируем в хостовый порядок для вывода
    sprintf_s(buffer, "%d.%d.%d.%d",
        (ip_host >> 24) & 0xFF,
        (ip_host >> 16) & 0xFF,
        (ip_host >> 8) & 0xFF,
        ip_host & 0xFF
    );
    return std::string(buffer);
}

// Вывод справки по использованию программы
void print_usage() {
    printf("Usage: mytraceroute <IP_address>\n\n");
    printf("Examples:\n");
    printf("  mytraceroute 8.8.8.8\n");
    printf("  mytraceroute 192.168.1.1\n");
}