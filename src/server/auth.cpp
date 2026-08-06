#include "auth.hpp"

#include <sodium.h>
#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

void ensureSodiumInitialized() {
  if (sodium_init() < 0) {
    throw std::runtime_error("Failed to initialize libsodium");
  }
}

}  // namespace

AuthManager::AuthManager() : database_(nullptr) {
  ensureSodiumInitialized();

  namespace filesystem = std::filesystem;
  if (!filesystem::exists("data")) {
    filesystem::create_directory("data");
  }

  const std::string databasePath = filesystem::absolute("data/chat.db").string();
  if (sqlite3_open(databasePath.c_str(), &database_) != SQLITE_OK) {
    const std::string message = database_ ? sqlite3_errmsg(database_) : "Unable to open database";
    if (database_ != nullptr) {
      sqlite3_close(database_);
      database_ = nullptr;
    }
    throw std::runtime_error(message);
  }

  constexpr char kCreateUsersTableSql[] =
      "CREATE TABLE IF NOT EXISTS users ("
      "username TEXT PRIMARY KEY, "
      "password_hash TEXT)";

  constexpr char kCreateRoomsTableSql[] =
      "CREATE TABLE IF NOT EXISTS rooms ("
      "room_id TEXT PRIMARY KEY)";

    constexpr char kCreateMessagesTableSql[] =
      "CREATE TABLE IF NOT EXISTS messages ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "room_id TEXT NOT NULL, "
      "sender_name TEXT NOT NULL, "
      "content TEXT NOT NULL, "
      "timestamp TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)";

  char* errorMessage = nullptr;
  if (sqlite3_exec(database_, kCreateUsersTableSql, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
    std::string message = errorMessage != nullptr ? errorMessage : "Failed to create users table";
    sqlite3_free(errorMessage);
    sqlite3_close(database_);
    database_ = nullptr;
    throw std::runtime_error(message);
  }

  errorMessage = nullptr;
  if (sqlite3_exec(database_, kCreateRoomsTableSql, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
    std::string message = errorMessage != nullptr ? errorMessage : "Failed to create rooms table";
    sqlite3_free(errorMessage);
    sqlite3_close(database_);
    database_ = nullptr;
    throw std::runtime_error(message);
  }

  errorMessage = nullptr;
  if (sqlite3_exec(database_, kCreateMessagesTableSql, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
    std::string message = errorMessage != nullptr ? errorMessage : "Failed to create messages table";
    sqlite3_free(errorMessage);
    sqlite3_close(database_);
    database_ = nullptr;
    throw std::runtime_error(message);
  }
}

AuthManager::~AuthManager() {
  if (database_ != nullptr) {
    sqlite3_close(database_);
    database_ = nullptr;
  }
}

bool AuthManager::registerUser(std::string username, std::string password) {
  if (database_ == nullptr) {
    return false;
  }

  char passwordHash[crypto_pwhash_STRBYTES];
  if (crypto_pwhash_str(passwordHash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_MIN,
                        crypto_pwhash_MEMLIMIT_MIN) != 0) {
    return false;
  }

  constexpr char kInsertUserSql[] =
      "INSERT INTO users (username, password_hash) VALUES (?, ?)";

  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database_, kInsertUserSql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(statement, 1, username.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, passwordHash, -1, SQLITE_TRANSIENT);

  const int stepResult = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return stepResult == SQLITE_DONE;
}

bool AuthManager::authenticateUser(std::string username, std::string password) {
  if (database_ == nullptr) {
    return false;
  }

  constexpr char kSelectUserSql[] = "SELECT password_hash FROM users WHERE username = ?";

  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database_, kSelectUserSql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(statement, 1, username.c_str(), -1, SQLITE_TRANSIENT);

  const int stepResult = sqlite3_step(statement);
  if (stepResult != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return false;
  }

  const unsigned char* storedHashText = sqlite3_column_text(statement, 0);
  if (storedHashText == nullptr) {
    sqlite3_finalize(statement);
    return false;
  }

  const bool matches = crypto_pwhash_str_verify(reinterpret_cast<const char*>(storedHashText), password.c_str(),
                                                password.size()) == 0;

  if (!matches) {
    sqlite3_finalize(statement);
    return false;
  }

  sqlite3_finalize(statement);
  return true;
}

bool AuthManager::createRoom(const std::string& room_id) {
  if (database_ == nullptr) {
    return false;
  }

  constexpr char kInsertRoomSql[] = "INSERT INTO rooms (room_id) VALUES (?);";

  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database_, kInsertRoomSql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(statement, 1, room_id.c_str(), -1, SQLITE_TRANSIENT);

  const int stepResult = sqlite3_step(statement);
  sqlite3_finalize(statement);

  if (stepResult == SQLITE_DONE) {
    return true;
  }

  if (stepResult == SQLITE_CONSTRAINT) {
    return false;
  }

  return false;
}

std::vector<std::string> AuthManager::getAllRooms() {
  std::vector<std::string> rooms;
  if (database_ == nullptr) {
    return rooms;
  }

  constexpr char kSelectRoomsSql[] = "SELECT room_id FROM rooms;";

  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database_, kSelectRoomsSql, -1, &statement, nullptr) != SQLITE_OK) {
    return rooms;
  }

  while (true) {
    const int stepResult = sqlite3_step(statement);
    if (stepResult == SQLITE_ROW) {
      const unsigned char* roomText = sqlite3_column_text(statement, 0);
      if (roomText != nullptr) {
        rooms.emplace_back(reinterpret_cast<const char*>(roomText));
      }
      continue;
    }

    break;
  }

  sqlite3_finalize(statement);
  return rooms;
}