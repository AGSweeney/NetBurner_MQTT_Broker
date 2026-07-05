// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_AUTH_STORE_H
#define MQTT_AUTH_STORE_H

#include <stddef.h>
#include <stdint.h>

// EFFS-backed MQTT client credential store. Passwords are stored as
// SHA-256(salt || password) with a random per-user salt; plaintext is never
// written to flash. All functions are safe to call from multiple tasks.

static const int kMqttAuthMaxUsers = 16;
static const int kMqttAuthMaxUserLen = 32;  // incl. NUL terminator

// Loads credentials from EFFS into memory. Safe to call when no file exists (0 users).
void MqttAuthStoreInit();

// Adds a new user or updates the password of an existing one. Generates a fresh
// salt on every call. Returns false when username/password is invalid or the
// user table is full.
bool MqttAuthSetUser(const char *username, const char *password, size_t password_len);
// Removes a user by name. Returns false when the username is missing.
bool MqttAuthDeleteUser(const char *username);
// Verifies raw password bytes (MQTT CONNECT payload, not a C string). Constant-time compare.
bool MqttAuthVerify(const char *username, const uint8_t *password, size_t password_len);

// Returns the number of stored users (0..kMqttAuthMaxUsers).
int MqttAuthUserCount();
// Copies the username at list index (0..count-1) into out. Returns false when
// the index is out of range.
bool MqttAuthUserAt(int index, char *out, size_t out_len);

#endif
