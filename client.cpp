#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main() {
    std::string server_ip;
    std::string port_text;
    std::string client_name;

    std::cout << "Enter server IP address: ";
    std::getline(std::cin, server_ip);

    std::cout << "Enter server port: ";
    std::getline(std::cin, port_text);

    std::cout << "Enter your name: ";
    std::getline(std::cin, client_name);

    int port = 0;
    try {
        port = std::stoi(port_text);
    } catch (const std::exception&) {
        std::cerr << "Invalid port number.\n";
        return 1;
    }

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, server_ip.c_str(), &server_address.sin_addr) <= 0) {
        std::cerr << "Invalid IP address.\n";
        close(client_socket);
        return 1;
    }

    if (connect(client_socket, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        std::perror("connect");
        close(client_socket);
        return 1;
    }

    std::string handshake = client_name + "\n";
    if (write(client_socket, handshake.c_str(), handshake.size()) == -1) {
        std::perror("write");
        close(client_socket);
        return 1;
    }

    std::atomic<bool> running{true};

    std::thread listener([&]() {
        char buffer[1024];
        while (running.load()) {
            ssize_t bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                std::cout << buffer << std::flush;
                continue;
            }

            if (bytes_read == 0) {
                if (running.exchange(false)) {
                    std::cout << "\nServer disconnected.\n";
                    shutdown(client_socket, SHUT_RDWR);
                }
                break;
            }

            if (running.exchange(false)) {
                std::perror("read");
                shutdown(client_socket, SHUT_RDWR);
            }
            break;
        }
    });

    std::string msg;
    while (running.load() && std::getline(std::cin, msg)) {
        if (msg == "exit") {
            running.store(false);
            shutdown(client_socket, SHUT_RDWR);
            break;
        }

        std::string outbound = msg + "\n";
        if (write(client_socket, outbound.c_str(), outbound.size()) == -1) {
            std::perror("write");
            running.store(false);
            shutdown(client_socket, SHUT_RDWR);
            break;
        }
    }

    if (running.exchange(false)) {
        shutdown(client_socket, SHUT_RDWR);
    }

    if (listener.joinable()) {
        listener.join();
    }

    close(client_socket);
    return 0;
}