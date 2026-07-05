// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// NetBurner EFFS-backed MQTT credential store. Keeps an in-memory user table
// synchronized with the "MQTTUSERS" flash file; mutations delete-then-rewrite
// the whole file under a critical section.

#include "mqtt_auth_store.h"

#include <crypto/ssl.h>
#include <crypto/wolfssl/wolfcrypt/settings.h>
#include <crypto/wolfssl/wolfcrypt/sha256.h>

#include <file/fsf.h>
#include <nbrtos.h>
#include <random.h>
#include <stdio.h>
#include <string.h>

static const char kAuthFileName[] = "MQTTUSERS";
static const uint32_t kAuthFileMagic = 0x4D515541;  // "MQUA"
static const uint32_t kAuthFileVersion = 1;
static const size_t kSaltLen = 16;
static const size_t kHashLen = 32;  // SHA-256 output

// One persisted user entry: username plus salted password hash.
struct AuthRecord {
    char username[kMqttAuthMaxUserLen];
    uint8_t salt[kSaltLen];
    uint8_t hash[kHashLen];
};

// On-disk file prefix; followed by count AuthRecord structs.
struct AuthFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
};

static AuthRecord gUsers[kMqttAuthMaxUsers];
static int gUserCount = 0;
static OS_CRIT gAuthCrit;  // guards gUsers/gUserCount and file I/O

// Computes SHA-256(salt || password). password may be empty (len 0).
static void hash_password(const uint8_t *salt, const uint8_t *password, size_t password_len,
                          uint8_t *out_hash)
{
    wc_Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, salt, kSaltLen);
    if (password != nullptr && password_len > 0) {
        wc_Sha256Update(&sha, password, static_cast<word32>(password_len));
    }
    wc_Sha256Final(&sha, out_hash);
    wc_Sha256Free(&sha);
}

// Returns index in gUsers, or -1 when username is not found.
static int find_user(const char *username)
{
    for (int i = 0; i < gUserCount; ++i) {
        if (strcmp(gUsers[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

// Persists the current in-memory table. Caller must hold gAuthCrit.
static bool save_store_locked()
{
    int rc = fs_delete(kAuthFileName);
    if (rc != FS_NOERR && rc != F_ERR_NOTFOUND) {
        iprintf("[MQTT AUTH] store delete error: %d\r\n", rc);
    }

    FS_FILE *fp = fs_open(kAuthFileName, "w");
    if (fp == nullptr) {
        iprintf("[MQTT AUTH] store open for write failed\r\n");
        return false;
    }

    AuthFileHeader hdr = {kAuthFileMagic, kAuthFileVersion, static_cast<uint32_t>(gUserCount)};
    bool ok = fs_write(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
    if (ok && gUserCount > 0) {
        size_t bytes = sizeof(AuthRecord) * static_cast<size_t>(gUserCount);
        ok = fs_write(gUsers, 1, bytes, fp) == static_cast<long>(bytes);
    }
    fs_close(fp);
    if (!ok) {
        iprintf("[MQTT AUTH] store write failed\r\n");
    }
    return ok;
}

void MqttAuthStoreInit()
{
    OSCriticalSectionObj lock(gAuthCrit);
    gUserCount = 0;

    FS_FILE *fp = fs_open(kAuthFileName, "r");
    if (fp == nullptr) {
        iprintf("[MQTT AUTH] no credential file (0 users)\r\n");
        return;
    }

    AuthFileHeader hdr = {};
    long n = fs_read(&hdr, 1, sizeof(hdr), fp);
    if (n != sizeof(hdr) || hdr.magic != kAuthFileMagic || hdr.version != kAuthFileVersion ||
        hdr.count > static_cast<uint32_t>(kMqttAuthMaxUsers)) {
        iprintf("[MQTT AUTH] credential file invalid; ignoring\r\n");
        fs_close(fp);
        return;
    }

    size_t bytes = sizeof(AuthRecord) * hdr.count;
    if (bytes > 0 && fs_read(gUsers, 1, bytes, fp) != static_cast<long>(bytes)) {
        iprintf("[MQTT AUTH] credential file truncated; ignoring\r\n");
        fs_close(fp);
        return;
    }
    fs_close(fp);

    gUserCount = static_cast<int>(hdr.count);
    iprintf("[MQTT AUTH] loaded %d user(s)\r\n", gUserCount);
}

bool MqttAuthSetUser(const char *username, const char *password, size_t password_len)
{
    if (username == nullptr || username[0] == '\0' ||
        strlen(username) >= kMqttAuthMaxUserLen || password == nullptr) {
        return false;
    }

    OSCriticalSectionObj lock(gAuthCrit);

    int idx = find_user(username);
    if (idx < 0) {
        if (gUserCount >= kMqttAuthMaxUsers) {
            return false;
        }
        idx = gUserCount++;
        memset(&gUsers[idx], 0, sizeof(AuthRecord));
        snprintf(gUsers[idx].username, sizeof(gUsers[idx].username), "%s", username);
    }

    for (size_t i = 0; i < kSaltLen; ++i) {
        gUsers[idx].salt[i] = GetRandomByte();
    }
    hash_password(gUsers[idx].salt, reinterpret_cast<const uint8_t *>(password), password_len,
                  gUsers[idx].hash);
    return save_store_locked();
}

bool MqttAuthDeleteUser(const char *username)
{
    if (username == nullptr || username[0] == '\0') {
        return false;
    }

    OSCriticalSectionObj lock(gAuthCrit);

    int idx = find_user(username);
    if (idx < 0) {
        return false;
    }
    for (int i = idx; i + 1 < gUserCount; ++i) {
        gUsers[i] = gUsers[i + 1];
    }
    gUserCount--;
    memset(&gUsers[gUserCount], 0, sizeof(AuthRecord));
    return save_store_locked();
}

bool MqttAuthVerify(const char *username, const uint8_t *password, size_t password_len)
{
    if (username == nullptr || username[0] == '\0') {
        return false;
    }

    OSCriticalSectionObj lock(gAuthCrit);

    int idx = find_user(username);
    if (idx < 0) {
        return false;
    }

    uint8_t computed[kHashLen];
    hash_password(gUsers[idx].salt, password, password_len, computed);

    // Constant-time compare.
    uint8_t diff = 0;
    for (size_t i = 0; i < kHashLen; ++i) {
        diff |= static_cast<uint8_t>(computed[i] ^ gUsers[idx].hash[i]);
    }
    return diff == 0;
}

int MqttAuthUserCount()
{
    OSCriticalSectionObj lock(gAuthCrit);
    return gUserCount;
}

bool MqttAuthUserAt(int index, char *out, size_t out_len)
{
    if (out == nullptr || out_len == 0) {
        return false;
    }
    OSCriticalSectionObj lock(gAuthCrit);
    if (index < 0 || index >= gUserCount) {
        return false;
    }
    snprintf(out, out_len, "%s", gUsers[index].username);
    return true;
}
