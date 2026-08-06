#pragma once

#include <string>
#include <vector>

struct sqlite3;

class AuthManager {
public:
  AuthManager();
  ~AuthManager();

  AuthManager(const AuthManager&) = delete;
  AuthManager& operator=(const AuthManager&) = delete;

  bool registerUser(std::string username, std::string password);
  bool authenticateUser(std::string username, std::string password);
  bool createRoom(const std::string& room_id);
  std::vector<std::string> getAllRooms();
  sqlite3* database() const noexcept { return database_; }

private:
  sqlite3* database_;
};