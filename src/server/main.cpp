#include "auth.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kListenPort = 8080;
constexpr std::size_t kBufferSize = 4096;

bool setNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool sendAll(int fd, const std::string& payload) {
  std::size_t totalSent = 0;
  while (totalSent < payload.size()) {
    const ssize_t sent = send(fd, payload.data() + totalSent, payload.size() - totalSent, 0);
    if (sent > 0) {
      totalSent += static_cast<std::size_t>(sent);
      continue;
    }

    if (sent == -1 && errno == EINTR) {
      continue;
    }

    return false;
  }

  return true;
}

bool sendPacket(int fd, const Packet& packet) {
  const nlohmann::json jsonPacket = packet;
  const std::string wirePayload = jsonPacket.dump() + '\n';
  return sendAll(fd, wirePayload);
}

bool sendJsonPacket(int fd, const nlohmann::json& packet) {
  const std::string wirePayload = packet.dump() + '\n';
  return sendAll(fd, wirePayload);
}

void sendStatusPacket(int fd, MessageType type, const std::string& payload) {
  Packet packet{};
  packet.type = type;
  packet.payload = payload;
  sendPacket(fd, packet);
}

bool storeMessage(sqlite3* db,
                  const std::string& roomId,
                  const std::string& senderName,
                  const std::string& content) {
  if (db == nullptr) {
    return false;
  }

  constexpr char kInsertMessageSql[] =
      "INSERT INTO messages (room_id, sender_name, content) VALUES (?, ?, ?)";

  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, kInsertMessageSql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(statement, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, senderName.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 3, content.c_str(), -1, SQLITE_TRANSIENT);

  const int stepResult = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return stepResult == SQLITE_DONE;
}

nlohmann::json get_recent_room_messages(sqlite3* db, const std::string& roomId) {
  nlohmann::json messages = nlohmann::json::array();
  if (db == nullptr) {
    return messages;
  }

  constexpr char kSelectRecentMessagesSql[] =
      "SELECT sender_name, content FROM ("
      "  SELECT sender_name, content, timestamp"
      "  FROM messages"
      "  WHERE room_id = ?"
      "  ORDER BY timestamp DESC"
      "  LIMIT 100"
      ") ORDER BY timestamp ASC";

  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, kSelectRecentMessagesSql, -1, &statement, nullptr) != SQLITE_OK) {
    return messages;
  }

  sqlite3_bind_text(statement, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);

  while (true) {
    const int stepResult = sqlite3_step(statement);
    if (stepResult == SQLITE_ROW) {
      const unsigned char* senderText = sqlite3_column_text(statement, 0);
      const unsigned char* contentText = sqlite3_column_text(statement, 1);
      messages.push_back({
          {"sender", senderText != nullptr ? reinterpret_cast<const char*>(senderText) : ""},
          {"content", contentText != nullptr ? reinterpret_cast<const char*>(contentText) : ""},
      });
      continue;
    }

    break;
  }

  sqlite3_finalize(statement);
  return messages;
}

void sendRoomHistoryPacket(int fd, const std::string& roomId, sqlite3* db) {
  nlohmann::json packet = {
      {"type", "room_history"},
      {"room", roomId},
      {"messages", get_recent_room_messages(db, roomId)},
  };
  sendJsonPacket(fd, packet);
}

void hydrate_rooms_from_db(sqlite3* db, std::unordered_map<std::string, std::unordered_set<int>>& active_rooms) {
  if (db == nullptr) {
    return;
  }

  auto hydrate_with_query = [&](const char* sql, int room_name_column) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
      return false;
    }

    while (true) {
      const int stepResult = sqlite3_step(statement);
      if (stepResult == SQLITE_ROW) {
        const unsigned char* roomText = sqlite3_column_text(statement, room_name_column);
        if (roomText != nullptr) {
          active_rooms.emplace(reinterpret_cast<const char*>(roomText), std::unordered_set<int>{});
        }
        continue;
      }

      break;
    }

    sqlite3_finalize(statement);
    return true;
  };

  if (hydrate_with_query("SELECT id, name FROM rooms;", 1)) {
    return;
  }

  hydrate_with_query("SELECT room_id FROM rooms;", 0);
}

void removeClient(std::vector<pollfd>& pollFds,
                  std::unordered_map<int, std::size_t>& fdToPollIndex,
                  int clientFd,
                  std::unordered_map<int, std::string>& activeSessions,
                  std::unordered_map<int, std::string>& activeRooms,
                  std::unordered_map<int, std::string>& inboundBuffers,
                  std::unordered_map<std::string, std::unordered_set<int>>& rooms,
                  std::shared_mutex& roomsMutex) {
  const auto indexIt = fdToPollIndex.find(clientFd);
  if (indexIt != fdToPollIndex.end()) {
    const std::size_t removeIndex = indexIt->second;
    const std::size_t lastIndex = pollFds.size() - 1;

    if (removeIndex != lastIndex) {
      std::swap(pollFds[removeIndex], pollFds[lastIndex]);
      fdToPollIndex[pollFds[removeIndex].fd] = removeIndex;
    }

    pollFds.pop_back();
    fdToPollIndex.erase(indexIt);
  }

  activeSessions.erase(clientFd);
  activeRooms.erase(clientFd);
  inboundBuffers.erase(clientFd);

  {
    std::unique_lock<std::shared_mutex> lock(roomsMutex);
    for (auto& [roomName, members] : rooms) {
      members.erase(clientFd);
    }
  }

  close(clientFd);
}

void addClient(std::vector<pollfd>& pollFds,
               std::unordered_map<int, std::size_t>& fdToPollIndex,
               int clientFd) {
  pollfd clientPollFd{};
  clientPollFd.fd = clientFd;
  clientPollFd.events = POLLIN | POLLERR | POLLHUP;
  clientPollFd.revents = 0;

  fdToPollIndex[clientFd] = pollFds.size();
  pollFds.push_back(clientPollFd);
}

void handleLogin(int clientFd,
                 const Packet& packet,
                 AuthManager& authManager,
                 std::unordered_map<int, std::string>& activeSessions) {
  if (!authManager.authenticateUser(packet.username, packet.payload)) {
    sendStatusPacket(clientFd, MessageType::LOGIN_FAIL, "Invalid username or password");
    return;
  }

  activeSessions[clientFd] = packet.username;

  Packet successPacket{MessageType::LOGIN_SUCCESS, packet.username, {}, "Login successful"};
  sendPacket(clientFd, successPacket);
}

void handleRegister(int clientFd, const Packet& packet, AuthManager& authManager) {
  if (packet.username.empty() || packet.payload.empty()) {
    sendStatusPacket(clientFd, MessageType::REGISTER_FAIL, "Username and password are required");
    return;
  }

  if (!authManager.registerUser(packet.username, packet.payload)) {
    sendStatusPacket(clientFd, MessageType::REGISTER_FAIL, "Registration failed");
    return;
  }

  sendStatusPacket(clientFd, MessageType::REGISTER_SUCCESS, "Registration successful");
}

void handleJoinRoom(int clientFd,
                    const Packet& packet,
                    AuthManager& authManager,
                    const std::unordered_map<int, std::string>& activeSessions,
                    std::unordered_map<int, std::string>& activeRooms,
                    std::unordered_map<std::string, std::unordered_set<int>>& rooms,
                    std::shared_mutex& roomsMutex) {
  if (!activeSessions.contains(clientFd)) {
    sendStatusPacket(clientFd, MessageType::JOIN_ROOM_FAIL, "Authentication required");
    return;
  }

  if (packet.room.empty()) {
    sendStatusPacket(clientFd, MessageType::JOIN_ROOM_FAIL, "Room name required");
    return;
  }

  {
    std::unique_lock<std::shared_mutex> lock(roomsMutex);
    const auto roomIt = rooms.find(packet.room);
    if (roomIt == rooms.end()) {
      sendStatusPacket(clientFd, MessageType::JOIN_ROOM_FAIL, "Room not found");
      return;
    }

    const auto currentRoomIt = activeRooms.find(clientFd);
    if (currentRoomIt != activeRooms.end()) {
      const auto previousRoomIt = rooms.find(currentRoomIt->second);
      if (previousRoomIt != rooms.end()) {
        previousRoomIt->second.erase(clientFd);
      }
    }

    roomIt->second.insert(clientFd);
    activeRooms[clientFd] = packet.room;
  }

  Packet successPacket{};
  successPacket.type = MessageType::JOIN_ROOM_SUCCESS;
  successPacket.room = packet.room;
  successPacket.payload = "Joined room";
  sendPacket(clientFd, successPacket);

  sendRoomHistoryPacket(clientFd, packet.room, authManager.database());
}

void handleCreateRoom(int clientFd,
                     const Packet& packet,
                     AuthManager& authManager,
                     const std::unordered_map<int, std::string>& activeSessions,
                     std::unordered_map<int, std::string>& activeRooms,
                     std::unordered_map<std::string, std::unordered_set<int>>& rooms,
                     std::shared_mutex& roomsMutex) {
  if (!activeSessions.contains(clientFd)) {
    sendStatusPacket(clientFd, MessageType::CREATE_ROOM_FAIL, "Authentication required");
    return;
  }

  if (packet.room.empty()) {
    sendStatusPacket(clientFd, MessageType::CREATE_ROOM_FAIL, "Room name required");
    return;
  }

  {
    std::unique_lock<std::shared_mutex> lock(roomsMutex);
    if (rooms.contains(packet.room)) {
      sendStatusPacket(clientFd, MessageType::CREATE_ROOM_FAIL, "Room already exists");
      return;
    }

    if (!authManager.createRoom(packet.room)) {
      sendStatusPacket(clientFd, MessageType::CREATE_ROOM_FAIL, "Room already exists");
      return;
    }

    const auto currentRoomIt = activeRooms.find(clientFd);
    if (currentRoomIt != activeRooms.end()) {
      const auto previousRoomIt = rooms.find(currentRoomIt->second);
      if (previousRoomIt != rooms.end()) {
        previousRoomIt->second.erase(clientFd);
      }
    }

    rooms.emplace(packet.room, std::unordered_set<int>{clientFd});
    activeRooms[clientFd] = packet.room;
  }

  Packet successPacket{};
  successPacket.type = MessageType::CREATE_ROOM_SUCCESS;
  successPacket.room = packet.room;
  successPacket.payload = "Room created";
  sendPacket(clientFd, successPacket);
}

void handleLeaveRoom(int clientFd,
                     const std::unordered_map<int, std::string>& activeSessions,
                     std::unordered_map<int, std::string>& activeRooms,
                     std::unordered_map<std::string, std::unordered_set<int>>& rooms,
                     std::shared_mutex& roomsMutex) {
  if (!activeSessions.contains(clientFd)) {
    return;
  }

  {
    std::unique_lock<std::shared_mutex> lock(roomsMutex);
    const auto currentRoomIt = activeRooms.find(clientFd);
    if (currentRoomIt != activeRooms.end()) {
      const auto roomIt = rooms.find(currentRoomIt->second);
      if (roomIt != rooms.end()) {
        roomIt->second.erase(clientFd);
      }
      activeRooms.erase(currentRoomIt);
      return;
    }

    for (auto& roomEntry : rooms) {
      roomEntry.second.erase(clientFd);
    }
  }
}

void handleMessage(int clientFd,
                   Packet packet,
                   AuthManager& authManager,
                   const std::unordered_map<int, std::string>& activeSessions,
                   std::unordered_map<std::string, std::unordered_set<int>>& rooms,
                   std::shared_mutex& roomsMutex) {
  const auto sessionIt = activeSessions.find(clientFd);
  if (sessionIt == activeSessions.end()) {
    return;
  }

  if (packet.room.empty()) {
    return;
  }

  packet.username = sessionIt->second;

  storeMessage(authManager.database(), packet.room, packet.username, packet.payload);

  std::vector<int> recipients;
  {
    std::shared_lock<std::shared_mutex> lock(roomsMutex);
    const auto roomIt = rooms.find(packet.room);
    if (roomIt == rooms.end()) {
      return;
    }

    recipients.assign(roomIt->second.begin(), roomIt->second.end());
  }

  const nlohmann::json jsonPacket = packet;
  const std::string wirePayload = jsonPacket.dump() + '\n';
  for (int recipientFd : recipients) {
    if (recipientFd == clientFd) {
      continue;
    }

    sendAll(recipientFd, wirePayload);
  }
}

void handlePacket(int clientFd,
                  const Packet& packet,
                  AuthManager& authManager,
                  std::unordered_map<int, std::string>& activeSessions,
                  std::unordered_map<int, std::string>& activeRooms,
                  std::unordered_map<std::string, std::unordered_set<int>>& rooms,
                  std::shared_mutex& roomsMutex) {
  switch (packet.type) {
    case MessageType::REGISTER:
      handleRegister(clientFd, packet, authManager);
      break;
    case MessageType::LOGIN:
      handleLogin(clientFd, packet, authManager, activeSessions);
      break;
    case MessageType::CREATE_ROOM:
      handleCreateRoom(clientFd, packet, authManager, activeSessions, activeRooms, rooms, roomsMutex);
      break;
    case MessageType::JOIN_ROOM:
      handleJoinRoom(clientFd, packet, authManager, activeSessions, activeRooms, rooms, roomsMutex);
      break;
    case MessageType::LEAVE_ROOM:
      handleLeaveRoom(clientFd, activeSessions, activeRooms, rooms, roomsMutex);
      break;
    case MessageType::MESSAGE:
      handleMessage(clientFd, packet, authManager, activeSessions, rooms, roomsMutex);
      break;
    case MessageType::REGISTER_SUCCESS:
    case MessageType::REGISTER_FAIL:
    case MessageType::LOGIN_SUCCESS:
    case MessageType::LOGIN_FAIL:
    case MessageType::CREATE_ROOM_SUCCESS:
    case MessageType::CREATE_ROOM_FAIL:
    case MessageType::JOIN_ROOM_SUCCESS:
    case MessageType::JOIN_ROOM_FAIL:
      break;
  }
}

void processIncomingData(int clientFd,
                         std::string& buffer,
                         AuthManager& authManager,
                         std::unordered_map<int, std::string>& activeSessions,
                         std::unordered_map<int, std::string>& activeRooms,
                         std::unordered_map<std::string, std::unordered_set<int>>& rooms,
                         std::shared_mutex& roomsMutex) {
  for (;;) {
    const std::size_t delimiterPosition = buffer.find('\n');
    if (delimiterPosition == std::string::npos) {
      break;
    }

    std::string frame = buffer.substr(0, delimiterPosition);
    buffer.erase(0, delimiterPosition + 1);

    if (frame.empty()) {
      continue;
    }

    try {
      const nlohmann::json jsonPacket = nlohmann::json::parse(frame);
      const Packet packet = jsonPacket.get<Packet>();
      handlePacket(clientFd, packet, authManager, activeSessions, activeRooms, rooms, roomsMutex);
    } catch (const std::exception& ex) {
      std::cerr << "Malformed packet from fd " << clientFd << ": " << ex.what() << '\n';
    }
  }
}

int createListeningSocket() {
  const int listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd == -1) {
    throw std::runtime_error("Failed to create listening socket");
  }

  int reuseAddress = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

  sockaddr_in serverAddress{};
  serverAddress.sin_family = AF_INET;
  serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
  serverAddress.sin_port = htons(kListenPort);

  if (bind(listenFd, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == -1) {
    close(listenFd);
    throw std::runtime_error("Failed to bind listening socket");
  }

  if (!setNonBlocking(listenFd)) {
    close(listenFd);
    throw std::runtime_error("Failed to set listening socket non-blocking");
  }

  if (listen(listenFd, SOMAXCONN) == -1) {
    close(listenFd);
    throw std::runtime_error("Failed to listen on socket");
  }

  return listenFd;
}

}  // namespace

int main() {
  try {
    signal(SIGPIPE, SIG_IGN);

    AuthManager authManager;

    const int listenFd = createListeningSocket();

    std::vector<pollfd> pollFds;
    pollFds.reserve(64);

    std::unordered_map<int, std::size_t> fdToPollIndex;
    addClient(pollFds, fdToPollIndex, listenFd);
    pollFds[0].events = POLLIN;

    std::unordered_map<int, std::string> activeSessions;
    std::unordered_map<int, std::string> activeRooms;
    std::unordered_map<int, std::string> inboundBuffers;
    std::unordered_map<std::string, std::unordered_set<int>> rooms;
    std::shared_mutex roomsMutex;

    hydrate_rooms_from_db(authManager.database(), rooms);

    std::vector<pollfd> readyFds;

    for (;;) {
      readyFds = pollFds;
      if (poll(readyFds.data(), static_cast<nfds_t>(readyFds.size()), -1) == -1) {
        if (errno == EINTR) {
          continue;
        }

        throw std::runtime_error("poll failed");
      }

      for (std::size_t index = 0; index < readyFds.size();) {
        const pollfd& readyFd = readyFds[index];
        if (readyFd.revents == 0) {
          ++index;
          continue;
        }

        const int fd = readyFd.fd;

        if (fd == listenFd) {
          for (;;) {
            sockaddr_in clientAddress{};
            socklen_t clientLength = sizeof(clientAddress);
            const int clientFd = accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
            if (clientFd == -1) {
              if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
              }

              if (errno == EINTR) {
                continue;
              }

              throw std::runtime_error("Failed to accept client");
            }

            if (!setNonBlocking(clientFd)) {
              close(clientFd);
              continue;
            }

            addClient(pollFds, fdToPollIndex, clientFd);
            inboundBuffers.emplace(clientFd, std::string{});
          }

          ++index;
          continue;
        }

        if ((readyFd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
          removeClient(pollFds, fdToPollIndex, fd, activeSessions, activeRooms, inboundBuffers, rooms, roomsMutex);
          readyFds = pollFds;
          continue;
        }

        if ((readyFd.revents & POLLIN) == 0) {
          ++index;
          continue;
        }

        char bufferChunk[kBufferSize];
        for (;;) {
          const ssize_t bytesRead = recv(fd, bufferChunk, sizeof(bufferChunk), 0);
          if (bytesRead > 0) {
            std::string& inboundBuffer = inboundBuffers[fd];
            inboundBuffer.append(bufferChunk, static_cast<std::size_t>(bytesRead));
            processIncomingData(fd, inboundBuffer, authManager, activeSessions, activeRooms, rooms, roomsMutex);
            continue;
          }

          if (bytesRead == 0) {
            removeClient(pollFds, fdToPollIndex, fd, activeSessions, activeRooms, inboundBuffers, rooms, roomsMutex);
            readyFds = pollFds;
            break;
          }

          if (errno == EINTR) {
            continue;
          }

          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }

          removeClient(pollFds, fdToPollIndex, fd, activeSessions, activeRooms, inboundBuffers, rooms, roomsMutex);
          readyFds = pollFds;
          break;
        }

        if (fdToPollIndex.find(fd) == fdToPollIndex.end()) {
          continue;
        }

        ++index;
      }
    }

    close(listenFd);
  } catch (const std::exception& ex) {
    std::cerr << "Server fatal error: " << ex.what() << '\n';
    return 1;
  }

  return 0;
}