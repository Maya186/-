#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <csignal>
#endif

// === НАСТРОЙКИ ===
const uint16_t UDP_PORT = 9999;
const uint16_t TCP_PORT = 8080;
const std::string BROADCAST_IP = "127.255.255.255";

// === СОБСТВЕННЫЙ ПРОТОКОЛ ===
const uint8_t TYPE_JOIN   = 1;
const uint8_t TYPE_MSG    = 2;
const uint8_t TYPE_LEAVE  = 3;

struct MsgHeader {
    uint8_t  type;
    uint16_t len;
};

// === ГЛОБАЛЬНОЕ СОСТОЯНИЕ ===
std::mutex clients_mtx;
std::vector<int> client_fds;
std::vector<std::string> client_names;
std::atomic<bool> running{true};

// === УТИЛИТЫ ===
#ifdef _WIN32
void socket_close(int fd) { closesocket(fd); }
void socket_shutdown(int fd) { shutdown(fd, SD_BOTH); }
#else
void socket_close(int fd) { close(fd); }
void socket_shutdown(int fd) { shutdown(fd, SHUT_RDWR); }
void ignore_sigpipe() { signal(SIGPIPE, SIG_IGN); }
#endif

bool send_exact(int fd, const void* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int res = send(fd, (const char*)buf + sent, (int)(len - sent), 0);
        if (res <= 0) return false;
        sent += res;
    }
    return true;
}

bool recv_exact(int fd, void* buf, size_t len) {
    size_t recvd = 0;
    while (recvd < len) {
        int res = recv(fd, (char*)buf + recvd, (int)(len - recvd), 0);
        if (res <= 0) return false;
        recvd += res;
    }
    return true;
}

// === СЕРВЕР: UDP ОБНАРУЖЕНИЕ ===
void udp_discovery_thread() {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    #ifdef _WIN32
    if (udp_sock == INVALID_SOCKET) return;
    #else
    if (udp_sock < 0) return;
    #endif

    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(UDP_PORT);
    bind(udp_sock, (struct sockaddr*)&addr, sizeof(addr));

    char buf[256];
    struct sockaddr_in sender{};
    int sender_len = sizeof(sender);

    std::cout << "[SERVER] UDP Discovery listener started.\n";
    while(running) {
        int n = recvfrom(udp_sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&sender, &sender_len);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << "[UDP] Node announced: \"" << buf << "\" from " << inet_ntoa(sender.sin_addr) << "\n";
        }
    }
    socket_close(udp_sock);
}

// === СЕРВЕР: ОБРАБОТКА КЛИЕНТА ===
void client_handler(int fd, std::string name) {
    std::cout << "[SERVER] Handling client: " << name << " (fd: " << fd << ")\n";
    MsgHeader hdr;
    char payload[256];

    while(running) {
        if (!recv_exact(fd, &hdr, sizeof(hdr))) break;
        hdr.len = ntohs(hdr.len);
        if (hdr.len > 255 || !recv_exact(fd, payload, hdr.len)) break;
        payload[hdr.len] = '\0';

        std::cout << "[SERVER] Received from " << name << " [TYPE:" << (int)hdr.type << "]: " << payload << "\n";

        std::lock_guard<std::mutex> lock(clients_mtx);
        for (size_t i = 0; i < client_fds.size(); ++i) {
            if (client_fds[i] != fd) {
                std::string b_msg = name + ": " + std::string(payload);
                uint16_t b_len = (uint16_t)b_msg.length();
                MsgHeader b_hdr = {hdr.type, htons(b_len)};
                send_exact(client_fds[i], &b_hdr, sizeof(b_hdr));
                send_exact(client_fds[i], b_msg.c_str(), b_len);
            }
        }
        if (hdr.type == TYPE_LEAVE) break;
    }

    std::lock_guard<std::mutex> lock(clients_mtx);
    for (auto it = client_fds.begin(); it != client_fds.end(); ++it) {
        if (*it == fd) {
            client_fds.erase(it);
            client_names.erase(client_names.begin() + std::distance(client_fds.begin(), it));
            std::cout << "[SERVER] Client " << name << " disconnected.\n";
            break;
        }
    }
    socket_close(fd);
}

// === СЕРВЕР: TCP ПРИЁМ ===
void tcp_acceptor_thread() {
    int tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    #ifdef _WIN32
    if (tcp_sock == INVALID_SOCKET) return;
    #else
    if (tcp_sock < 0) return;
    #endif

    int opt = 1;
    setsockopt(tcp_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TCP_PORT);
    bind(tcp_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(tcp_sock, 10);
    std::cout << "[SERVER] TCP listener started on port " << TCP_PORT << "\n";

    while(running) {
        struct sockaddr_in cli_addr{};
        int cli_len = sizeof(cli_addr);
        int new_fd = accept(tcp_sock, (struct sockaddr*)&cli_addr, &cli_len);
        if (new_fd == -1) continue;

        MsgHeader hdr;
        char name_buf[64];
        if (recv_exact(new_fd, &hdr, sizeof(hdr)) && hdr.type == TYPE_JOIN) {
            uint16_t nlen = ntohs(hdr.len);
            if (recv_exact(new_fd, name_buf, nlen)) {
                name_buf[nlen] = '\0';
                std::string name(name_buf);
                std::lock_guard<std::mutex> lock(clients_mtx);
                client_fds.push_back(new_fd);
                client_names.push_back(name);
                std::thread(client_handler, new_fd, name).detach();
            }
        } else {
            socket_close(new_fd);
        }
    }
    socket_close(tcp_sock);
}

// === КЛИЕНТ ===
void run_client(std::string name, std::string local_ip) {
    // 1. UDP Broadcast с привязкой к определённому IP
    int udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));
    
    // Привязка к конкретному локальному IP для UDP
    struct sockaddr_in udp_local_addr{};
    udp_local_addr.sin_family = AF_INET;
    udp_local_addr.sin_addr.s_addr = inet_addr(local_ip.c_str());
    udp_local_addr.sin_port = 0;
    bind(udp_sock, (struct sockaddr*)&udp_local_addr, sizeof(udp_local_addr));

    struct sockaddr_in bcast_addr{};
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_addr.s_addr = inet_addr(BROADCAST_IP.c_str());
    bcast_addr.sin_port = htons(UDP_PORT);
    sendto(udp_sock, name.c_str(), (int)name.length(), 0, (struct sockaddr*)&bcast_addr, sizeof(bcast_addr));
    std::cout << "[CLIENT] Broadcasted name via UDP from " << local_ip << ": " << name << "\n";
    socket_close(udp_sock);

    // 2. TCP Connect с привязкой к определённому локальному IP
    int tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    
    // Привязка к конкретному локальному IP
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = inet_addr(local_ip.c_str());
    local_addr.sin_port = 0; // Любой свободный порт
    bind(tcp_sock, (struct sockaddr*)&local_addr, sizeof(local_addr));
    
    std::cout << "[CLIENT] Bound to local IP: " << local_ip << "\n";

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(TCP_PORT);

    if (connect(tcp_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        std::cerr << "[CLIENT] Connection failed from " << local_ip << "\n";
        socket_close(tcp_sock);
        return;
    }
    std::cout << "[CLIENT] TCP connected from " << local_ip << ". Handshake complete.\n";

    // 3. Отправка JOIN
    uint16_t nlen = (uint16_t)name.length();
    MsgHeader join_hdr = {TYPE_JOIN, htons(nlen)};
    send_exact(tcp_sock, &join_hdr, sizeof(join_hdr));
    send_exact(tcp_sock, name.c_str(), nlen);

    // 4. Поток приёма
    std::atomic<bool> client_alive{true};
    std::thread recv_thread([&]() {
        MsgHeader hdr;
        char payload[256];
        while(client_alive && running) {
            if (!recv_exact(tcp_sock, &hdr, sizeof(hdr))) break;
            hdr.len = ntohs(hdr.len);
            if (hdr.len > 255 || !recv_exact(tcp_sock, payload, hdr.len)) break;
            payload[hdr.len] = '\0';
            std::cout << "\n[CHAT] " << payload << "\n> " << std::flush;
        }
    });

    // 5. Цикл ввода
    std::string input;
    while(running) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, input)) break;
        if (input == "exit" || input == "quit") {
            std::string leave = "has left the chat.";
            uint16_t llen = (uint16_t)leave.length();
            MsgHeader l_hdr = {TYPE_LEAVE, htons(llen)};
            send_exact(tcp_sock, &l_hdr, sizeof(l_hdr));
            send_exact(tcp_sock, leave.c_str(), llen);
            break;
        }
        uint16_t mlen = (uint16_t)input.length();
        MsgHeader m_hdr = {TYPE_MSG, htons(mlen)};
        send_exact(tcp_sock, &m_hdr, sizeof(m_hdr));
        send_exact(tcp_sock, input.c_str(), mlen);
    }

    client_alive = false;
    socket_shutdown(tcp_sock);
    socket_close(tcp_sock);
    if (recv_thread.joinable()) recv_thread.join();
    std::cout << "[CLIENT] Session ended.\n";
}

int main(int argc, char* argv[]) {
    #ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }
    #else
    ignore_sigpipe();
    #endif

    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  Server: " << argv[0] << " server\n";
        std::cout << "  Client: " << argv[0] << " client <name> [local_ip]\n";
        std::cout << "  Example: " << argv[0] << " client Alice 127.0.0.2\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "server") {
        std::thread(udp_discovery_thread).detach();
        tcp_acceptor_thread();
    } else if (mode == "client") {
        if (argc < 3) { 
            std::cerr << "Client requires a name.\n"; 
            return 1; 
        }
        std::string name = argv[2];
        std::string local_ip = (argc > 3) ? argv[3] : "127.0.0.1";
        run_client(name, local_ip);
    } else {
        std::cerr << "Unknown mode.\n";
    }

    #ifdef _WIN32
    WSACleanup();
    #endif
    return 0;
}