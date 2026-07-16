#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

std::vector<int> client_sockets;
std::mutex clients_mutex;

void remove_client_socket(int client_socket) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = std::find(client_sockets.begin(), client_sockets.end(), client_socket);
    if (it != client_sockets.end()) {
        client_sockets.erase(it);
    }
}

std::string trim_trailing_newlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

void handle_client(int client_socket) {
    char buffer[1024];

    ssize_t bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        remove_client_socket(client_socket);
        close(client_socket);
        return;
    }

    buffer[bytes_read] = '\0';
    std::string client_name = trim_trailing_newlines(buffer);
    if (client_name.empty()) {
        client_name = "Unknown";
    }

    while (true) {
        bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            remove_client_socket(client_socket);
            close(client_socket);
            return;
        }

        buffer[bytes_read] = '\0';
        std::string message = trim_trailing_newlines(buffer);
        if (message.empty()) {
            continue;
        }

        std::string formatted_message = "[" + client_name + "]: " + message + "\n";

        std::lock_guard<std::mutex> lock(clients_mutex);
        for (int other_socket : client_sockets) {
            if (other_socket != client_socket) {
                send(other_socket, formatted_message.c_str(), formatted_message.size(), 0);
            }
        }
    }
}

int main() {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        std::perror("socket");
        return 1;
    }

    int reuse_address = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) == -1) {
        std::perror("setsockopt");
        close(server_socket);
        return 1;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(8080);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_socket, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        std::perror("bind");
        close(server_socket);
        return 1;
    }

    if (listen(server_socket, SOMAXCONN) == -1) {
        std::perror("listen");
        close(server_socket);
        return 1;
    }

    std::cout << "Relay chat server listening on port 8080..." << std::endl;

    while (true) {
        sockaddr_in client_address{};
        socklen_t client_address_length = sizeof(client_address);
        int client_socket = accept(server_socket, reinterpret_cast<sockaddr*>(&client_address), &client_address_length);
        if (client_socket == -1) {
            std::perror("accept");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            client_sockets.push_back(client_socket);
        }

        std::thread client_thread(handle_client, client_socket);
        client_thread.detach();
    }

    close(server_socket);
    return 0;
}
