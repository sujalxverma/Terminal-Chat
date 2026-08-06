#include "protocol.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace {

constexpr const char* kServerAddress = "127.0.0.1";
constexpr int kServerPort = 8080;

bool sendAll(int fd, const std::string& payload) {
  std::size_t total_sent = 0;
  while (total_sent < payload.size()) {
    const ssize_t sent = send(fd, payload.data() + total_sent, payload.size() - total_sent, 0);
    if (sent > 0) {
      total_sent += static_cast<std::size_t>(sent);
      continue;
    }

    if (sent == -1 && errno == EINTR) {
      continue;
    }

    return false;
  }

  return true;
}

bool sendPacket(int socket_fd, const Packet& packet) {
  const nlohmann::json json_packet = packet;
  const std::string wire_payload = json_packet.dump() + '\n';
  return sendAll(socket_fd, wire_payload);
}

int connectToServer() {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    throw std::runtime_error("Failed to create client socket");
  }

  sockaddr_in server_address{};
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(kServerPort);
  if (inet_pton(AF_INET, kServerAddress, &server_address.sin_addr) != 1) {
    close(socket_fd);
    throw std::runtime_error("Invalid server address");
  }

  if (connect(socket_fd, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == -1) {
    close(socket_fd);
    throw std::runtime_error("Failed to connect to server");
  }

  return socket_fd;
}

std::string formatMessage(const Packet& packet) {
  if (packet.username.empty()) {
    return packet.payload;
  }

  if (packet.room.empty()) {
    return packet.username + ": " + packet.payload;
  }

  return packet.room + " | " + packet.username + ": " + packet.payload;
}

void sendLoginPacket(int socket_fd, const std::string& username, const std::string& password) {
  Packet packet{};
  packet.type = MessageType::LOGIN;
  packet.username = username;
  packet.payload = password;
  sendPacket(socket_fd, packet);
}

void sendRegisterPacket(int socket_fd, const std::string& username, const std::string& password) {
  Packet packet{};
  packet.type = MessageType::REGISTER;
  packet.username = username;
  packet.payload = password;
  sendPacket(socket_fd, packet);
}

void sendJoinRoomPacket(int socket_fd, const std::string& room_id) {
  Packet packet{};
  packet.type = MessageType::JOIN_ROOM;
  packet.room = room_id;
  packet.payload = room_id;
  sendPacket(socket_fd, packet);
}

void sendCreateRoomPacket(int socket_fd, const std::string& room_id) {
  Packet packet{};
  packet.type = MessageType::CREATE_ROOM;
  packet.room = room_id;
  packet.payload = room_id;
  sendPacket(socket_fd, packet);
}

void sendLeaveRoomPacket(int socket_fd) {
  Packet packet{};
  packet.type = MessageType::LEAVE_ROOM;
  sendPacket(socket_fd, packet);
}

void sendMessagePacket(int socket_fd,
                       const std::string& username,
                       const std::string& room_id,
                       const std::string& message) {
  Packet packet{};
  packet.type = MessageType::MESSAGE;
  packet.username = username;
  packet.room = room_id;
  packet.payload = message;
  sendPacket(socket_fd, packet);
}

}  // namespace

int main() {
  try {
    std::signal(SIGPIPE, SIG_IGN);

    const int socket_fd = connectToServer();

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    auto exit_loop = screen.ExitLoopClosure();

    enum ViewState {
      AUTHENTICATION = 0,
      ROOM_SELECTION = 1,
      CHAT_ROOM = 2,
    };

    int current_view = AUTHENTICATION;
    bool authenticated = false;
    std::string status_message = "Connected to 127.0.0.1:8080";

    std::string login_username;
    std::string login_password;

    std::string room_id;
    std::string current_room_id;

    std::string chat_input_value;
    std::vector<std::string> chat_history;

    auto resetChatState = [&] {
      chat_history.clear();
      chat_input_value.clear();
    };

    auto login_username_input = ftxui::Input(&login_username, "Username");
    ftxui::InputOption login_password_options;
    login_password_options.password = true;
    auto login_password_input = ftxui::Input(&login_password, "Password", login_password_options);

    auto login_button = ftxui::Button("Login", [&] {
      if (login_username.empty() || login_password.empty()) {
        status_message = "Enter a username and password.";
        return;
      }

      sendLoginPacket(socket_fd, login_username, login_password);
    });

    auto register_button = ftxui::Button("Register", [&] {
      if (login_username.empty() || login_password.empty()) {
        status_message = "Enter a username and password.";
        return;
      }

      sendRegisterPacket(socket_fd, login_username, login_password);
    });

    auto login_buttons = ftxui::Container::Horizontal({login_button, register_button});
    auto login_container = ftxui::Container::Vertical({login_username_input, login_password_input, login_buttons});
  auto login_view = ftxui::Renderer(login_container, [&] {
    using namespace ftxui;

    return window(text(" Terminal-X : Authentication "),
          vbox({
            login_username_input->Render(),
            login_password_input->Render(),
            separator(),
            hbox({login_button->Render(), text(" "), register_button->Render()}),
            separator(),
            text(status_message) | dim,
          })) |
       center;
  });

    auto room_input = ftxui::Input(&room_id, "Room ID");

    auto join_button = ftxui::Button("Join Existing Room", [&] {
      if (room_id.empty()) {
        status_message = "Enter a room ID.";
        return;
      }

      current_room_id = room_id;
      resetChatState();
      sendJoinRoomPacket(socket_fd, room_id);
    });

    auto create_button = ftxui::Button("Create New Room", [&] {
      if (room_id.empty()) {
        status_message = "Enter a room ID.";
        return;
      }

      current_room_id = room_id;
      resetChatState();
      sendCreateRoomPacket(socket_fd, room_id);
    });

    auto room_buttons = ftxui::Container::Horizontal({join_button, create_button});
    auto room_container = ftxui::Container::Vertical({room_input, room_buttons});
  auto room_view = ftxui::Renderer(room_container, [&] {
    using namespace ftxui;

    return window(text(" Terminal-X : Rooms "),
          vbox({
            room_input->Render(),
            separator(),
            hbox({join_button->Render(), text(" "), create_button->Render()}),
            separator(),
            text(status_message) | dim,
          })) |
       center;
  });

    auto chat_history_view = ftxui::Renderer([&] {
      using namespace ftxui;

      Elements lines;
      lines.reserve(chat_history.size());
      for (const auto& line : chat_history) {
        lines.push_back(text(line));
      }

      if (lines.empty()) {
        lines.push_back(text("No messages yet.") | dim);
      }

      return vbox(std::move(lines)) | yframe | flex;
    });

    auto chat_input = ftxui::Input(&chat_input_value, "Message");
    chat_input = ftxui::CatchEvent(chat_input, [&](const ftxui::Event& event) {
      if (event != ftxui::Event::Return) {
        return false;
      }

      if (!authenticated || current_room_id.empty() || chat_input_value.empty()) {
        return true;
      }

      const std::string outgoing_message = chat_input_value;
      sendMessagePacket(socket_fd, login_username, current_room_id, outgoing_message);
      chat_history.push_back(login_username + ": " + outgoing_message);
      chat_input_value.clear();
      return true;
    });

    auto chat_input_box = ftxui::Renderer(chat_input, [&] {
      return chat_input->Render() | ftxui::border;
    });

    auto chat_container = ftxui::Container::Vertical({chat_history_view, chat_input_box});
    auto chat_view = ftxui::Renderer(chat_container, [&] {
      using namespace ftxui;

      return vbox({
             text("Terminal-X - Chat") | bold | center,
             separator(),
             chat_history_view->Render() | flex,
             separator(),
             chat_input_box->Render(),
             separator(),
             text("Room: " + (current_room_id.empty() ? std::string("<none>") : current_room_id)) | dim,
             }) |
             border;
    });

    auto main_container = ftxui::Container::Tab(ftxui::Components{login_view, room_view, chat_view}, &current_view);

    auto root = ftxui::CatchEvent(main_container, [&](const ftxui::Event& event) {
      if (event == ftxui::Event::Special("\x0b")) {
        screen.Exit();
        return true;
      }

      if (event == ftxui::Event::Escape) {
        if (current_view == CHAT_ROOM) {
          sendLeaveRoomPacket(socket_fd);
          current_room_id.clear();
          resetChatState();
          current_view = ROOM_SELECTION;
          status_message = "Left room.";
          return true;
        }

        return false;
      }

      return false;
    });

    std::atomic<bool> reader_running{true};
    std::thread reader_thread([&] {
      std::string buffer;
      char chunk[4096];

      while (reader_running.load()) {
        const ssize_t bytes_read = recv(socket_fd, chunk, sizeof(chunk), 0);
        if (bytes_read > 0) {
          buffer.append(chunk, static_cast<std::size_t>(bytes_read));

          for (;;) {
            const std::size_t delimiter = buffer.find('\n');
            if (delimiter == std::string::npos) {
              break;
            }

            const std::string frame = buffer.substr(0, delimiter);
            buffer.erase(0, delimiter + 1);
            if (frame.empty()) {
              continue;
            }

            try {
              const nlohmann::json json_packet = nlohmann::json::parse(frame);
              const std::string packet_type = json_packet.value("type", "");

              if (packet_type == "room_history") {
                screen.Post([&, history = json_packet.value("messages", nlohmann::json::array())] {
                  chat_history.clear();
                  chat_input_value.clear();

                  for (const auto& message : history) {
                    const std::string sender = message.value("sender", std::string{});
                    const std::string content = message.value("content", std::string{});
                    chat_history.push_back(sender + ": " + content);
                  }

                  if (current_view != CHAT_ROOM) {
                    current_view = CHAT_ROOM;
                  }
                });
                screen.PostEvent(ftxui::Event::Custom);
                continue;
              }

              const Packet packet = json_packet.get<Packet>();

              switch (packet.type) {
                case MessageType::REGISTER_SUCCESS:
                  screen.Post([&] {
                    status_message = "Registration successful.";
                  });
                  break;
                case MessageType::REGISTER_FAIL:
                  screen.Post([&, message = packet.payload] { status_message = message; });
                  break;
                case MessageType::LOGIN_SUCCESS:
                  screen.Post([&, packet] {
                    current_view = ROOM_SELECTION;
                    authenticated = true;
                    login_username = packet.username.empty() ? login_username : packet.username;
                    status_message = "Login successful.";
                  });
                  break;
                case MessageType::LOGIN_FAIL:
                  screen.Post([&, message = packet.payload] { status_message = message; });
                  break;
                case MessageType::CREATE_ROOM_SUCCESS:
                case MessageType::JOIN_ROOM_SUCCESS:
                  screen.Post([&, packet] {
                    current_view = CHAT_ROOM;
                    current_room_id = packet.room.empty() ? current_room_id : packet.room;
                    chat_history.clear();
                    chat_input_value.clear();
                    status_message = "Room ready: " + current_room_id;
                  });
                  break;
                case MessageType::CREATE_ROOM_FAIL:
                case MessageType::JOIN_ROOM_FAIL:
                  screen.Post([&, message = packet.payload] { status_message = message; });
                  break;
                case MessageType::LEAVE_ROOM:
                  break;
                case MessageType::MESSAGE:
                  screen.Post([&, line = formatMessage(packet)] {
                    chat_history.push_back(line);
                  });
                  screen.PostEvent(ftxui::Event::Custom);
                  break;
                case MessageType::REGISTER:
                case MessageType::LOGIN:
                case MessageType::CREATE_ROOM:
                case MessageType::JOIN_ROOM:
                  break;
              }
            } catch (const std::exception& ex) {
              screen.Post([&, message = std::string(ex.what())] { status_message = message; });
            }
          }

          continue;
        }

        if (bytes_read == 0) {
          screen.Post([&] { status_message = "Server disconnected."; });
          break;
        }

        if (errno == EINTR) {
          continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }

        screen.Post([&, message = std::string(std::strerror(errno))] {
          status_message = "Socket read failed: " + message;
        });
        break;
      }
    });

    screen.Loop(root);

    reader_running.store(false);
    shutdown(socket_fd, SHUT_RDWR);
    if (reader_thread.joinable()) {
      reader_thread.join();
    }
    close(socket_fd);
  } catch (const std::exception& ex) {
    std::cerr << "Client fatal error: " << ex.what() << '\n';
    return 1;
  }

  return 0;
}