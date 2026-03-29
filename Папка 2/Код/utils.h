#pragma once
#include <winsock2.h>
#include <string>

// Парсинг строки с IP-адресом в сетевой порядок байт
UINT32 parse_ip_address(const char* ip_str);

// Преобразование IP (в сетевом порядке) в строку
std::string ip_to_string(UINT32 ip);

// Вывод справки по использованию программы
void print_usage();