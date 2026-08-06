#pragma once

#include <nlohmann/json.hpp>

#include <string>

enum class MessageType {
  REGISTER,
  REGISTER_SUCCESS,
  REGISTER_FAIL,
  LOGIN,
  LOGIN_SUCCESS,
  LOGIN_FAIL,
  CREATE_ROOM,
  CREATE_ROOM_SUCCESS,
  CREATE_ROOM_FAIL,
  JOIN_ROOM,
  JOIN_ROOM_SUCCESS,
  JOIN_ROOM_FAIL,
  LEAVE_ROOM,
  MESSAGE,
};

struct Packet {
  MessageType type{};
  std::string username;
  std::string room;
  std::string payload;
};

NLOHMANN_JSON_SERIALIZE_ENUM(MessageType, {
  {MessageType::REGISTER, "REGISTER"},
  {MessageType::REGISTER_SUCCESS, "REGISTER_SUCCESS"},
  {MessageType::REGISTER_FAIL, "REGISTER_FAIL"},
  {MessageType::LOGIN, "LOGIN"},
  {MessageType::LOGIN_SUCCESS, "LOGIN_SUCCESS"},
  {MessageType::LOGIN_FAIL, "LOGIN_FAIL"},
  {MessageType::CREATE_ROOM, "CREATE_ROOM"},
  {MessageType::CREATE_ROOM_SUCCESS, "CREATE_ROOM_SUCCESS"},
  {MessageType::CREATE_ROOM_FAIL, "CREATE_ROOM_FAIL"},
  {MessageType::JOIN_ROOM, "JOIN_ROOM"},
  {MessageType::JOIN_ROOM_SUCCESS, "JOIN_ROOM_SUCCESS"},
  {MessageType::JOIN_ROOM_FAIL, "JOIN_ROOM_FAIL"},
  {MessageType::LEAVE_ROOM, "leave_room"},
  {MessageType::MESSAGE, "MESSAGE"},
})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Packet, type, username, room, payload)