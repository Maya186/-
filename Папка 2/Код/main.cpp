#include "icmp.h"
#include "utils.h"
#include <ws2tcpip.h>
#include <ctime>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

#define MAX_HOPS 30
#define TIMEOUT_MS 3000
#define PACKETS_PER_HOP 3
#define ICMP_DATA_SIZE 32

int send_icmp_echo(SOCKET sock, UINT32 dst_ip, UINT16 seq, UINT16 id);

int receive_reply(
    SOCKET sock,
    UINT32* reply_ip,
    UINT8* icmp_type,
    UINT8* icmp_code,
    UINT16* icmp_seq
);

int main(int argc, char* argv[]) {
    // Парсинг аргументов командной строки 
    if (argc != 2) {
        print_usage();
        return 1;
    }

    const char* target = argv[1];

    // Инициализация Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    // Парсинг IP-адреса 
    UINT32 dst_ip = parse_ip_address(target);
    if (dst_ip == INADDR_NONE) {
        fprintf(stderr, "Invalid IP address: %s\n", target);
        WSACleanup();
        return 1;
    }

    std::string dst_ip_str = ip_to_string(dst_ip);

    // Вывод заголовка
    printf("Tracing route to %s\n", dst_ip_str.c_str());
    printf("Over a maximum of %d hops:\n\n", MAX_HOPS);

    // Создаём raw socket для ICMP
    SOCKET sock = WSASocket(
        AF_INET,
        SOCK_RAW,
        IPPROTO_ICMP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED
    );

    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "Raw socket failed: %d\n", WSAGetLastError());
        fprintf(stderr, "Run as Administrator!\n");
        WSACleanup();
        return 1;
    }

    // Уникальный идентификатор для всех пакетов 
    UINT16 icmp_id = static_cast<UINT16>(GetCurrentProcessId() & 0xFFFF);

    bool target_reached = false;

    for (UINT8 ttl = 1; ttl <= MAX_HOPS; ttl++) {
        printf("%3d  ", ttl);

        // Устанавливаем TTL для IP заголовка
        setsockopt(sock, IPPROTO_IP, IP_TTL,
            reinterpret_cast<char*>(&ttl), sizeof(ttl));

        double rtts[PACKETS_PER_HOP];
        UINT32 reply_ips[PACKETS_PER_HOP];
        bool success[PACKETS_PER_HOP] = { false };

        // Отправляем 3 пакета и собираем результаты
        for (int probe = 0; probe < PACKETS_PER_HOP; probe++) {
            clock_t start = clock();

            int send_result = send_icmp_echo(sock, dst_ip, probe, icmp_id);

            if (send_result == SOCKET_ERROR) {
                rtts[probe] = -1;
                continue;
            }

            UINT32 reply_ip;
            UINT8 icmp_type, icmp_code;
            UINT16 icmp_seq;
            int recv_result = receive_reply(sock, &reply_ip, &icmp_type, &icmp_code, &icmp_seq);

            clock_t end = clock();
            double rtt = static_cast<double>(end - start) * 1000.0 / CLOCKS_PER_SEC;

            if (recv_result == SOCKET_ERROR) {
                rtts[probe] = -1;  // Таймаут
            }
            else {
                rtts[probe] = rtt;
                reply_ips[probe] = reply_ip;
                success[probe] = true;

                // Если получили Echo Reply - достигли цели
                if (icmp_type == ICMP_ECHO_REPLY) {
                    target_reached = true;
                }
            }
        }

        // Выводим времена отклика
        for (int probe = 0; probe < PACKETS_PER_HOP; probe++) {
            if (rtts[probe] < 0) {
                printf("   *  ");
            }
            else {
                printf("%4.0f ms  ", rtts[probe]);
            }
        }

        // Выводим адрес узла 
        for (int probe = PACKETS_PER_HOP - 1; probe >= 0; probe--) {
            if (success[probe]) {
                printf("%s", ip_to_string(reply_ips[probe]).c_str());
                break;
            }
        }

        printf("\n");

        // Если достигли цели - завершаем трассировку
        if (target_reached) {
            break;
        }
    }

    printf("\nTrace complete.\n");

    // Очистка ресурсов
    closesocket(sock);
    WSACleanup();

    return 0;
}

// Отправка одного ICMP Echo Request
int send_icmp_echo(SOCKET sock, UINT32 dst_ip, UINT16 seq, UINT16 id) {
    char packet[1024] = { 0 };

    // Заполняем ICMP заголовок
    ICMPHeader* icmp = (ICMPHeader*)packet;
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;  // Обнуляем перед расчётом
    icmp->identifier = htons(id);
    icmp->sequence = htons(seq);

    // Заполняем данные пакета
    char* data = packet + sizeof(ICMPHeader);
    for (int i = 0; i < ICMP_DATA_SIZE; i++) {
        data[i] = static_cast<char>(i);
    }

    // Считаем контрольную сумму
    icmp->checksum = calculate_checksum(
        reinterpret_cast<USHORT*>(packet),
        sizeof(ICMPHeader) + ICMP_DATA_SIZE
    );

    // Адрес назначения
    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = dst_ip;

    // Отправляем пакет
    return sendto(
        sock,
        packet,
        sizeof(ICMPHeader) + ICMP_DATA_SIZE,
        0,
        reinterpret_cast<sockaddr*>(&dest),
        sizeof(dest)
    );
}

// Получение и парсинг ответа
// Возвращает: размер пакета или SOCKET_ERROR при таймауте
int receive_reply(
    SOCKET sock,
    UINT32* reply_ip,
    UINT8* icmp_type,
    UINT8* icmp_code,
    UINT16* icmp_seq
) {
    char buffer[2048];
    sockaddr_in from;
    int fromlen = sizeof(from);

    // Устанавливаем таймаут получения
    int timeout = TIMEOUT_MS;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<char*>(&timeout), sizeof(timeout));

    // Получаем пакет
    int len = recvfrom(sock, buffer, sizeof(buffer), 0,
        reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (len == SOCKET_ERROR) {
        return SOCKET_ERROR;
    }

    // Парсим IP заголовок (минимум 20 байт)
    if (len < 20) return SOCKET_ERROR;

    IPHeader* ip = reinterpret_cast<IPHeader*>(buffer);
    *reply_ip = ip->src_ip;  // IP источника ответа

    // Длина IP заголовка в байтах (IHL * 4)
    int ip_header_len = (ip->ihl * 4);
    if (len < ip_header_len + 8) return SOCKET_ERROR;  // Минимум ещё 8 байт ICMP

    // Парсим ICMP заголовок (идёт сразу после IP)
    ICMPHeader* icmp = reinterpret_cast<ICMPHeader*>(buffer + ip_header_len);
    *icmp_type = icmp->type;
    *icmp_code = icmp->code;
    *icmp_seq = ntohs(icmp->sequence);

    return len;
}
