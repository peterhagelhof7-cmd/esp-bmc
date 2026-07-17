#include "user_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "storage_manager.h"

static const char* TAG = "user_manager";
#define USERS_FILE "/storage/users.json"

#define MAX_USERS 16
#define USERNAME_CAP 32
#define SALT_HEX_LEN 16   // 8 Byte Salt als Hex
#define HASH_HEX_LEN 64   // SHA-256 = 32 Byte als Hex

typedef struct {
  char username[USERNAME_CAP];
  char salt_hex[SALT_HEX_LEN + 1];
  char hash_hex[HASH_HEX_LEN + 1];
  user_role_t role;
  bool in_use;
} user_entry_t;

static user_entry_t s_users[MAX_USERS];
static size_t s_user_count;

#define MAX_SESSIONS 8
#define SESSION_TOKEN_HEX_LEN 32
#define SESSION_TTL_US (30LL * 60 * 1000 * 1000)  // 30 Minuten

typedef struct {
  char token_hex[SESSION_TOKEN_HEX_LEN + 1];
  char username[USERNAME_CAP];
  user_role_t role;
  int64_t expires_at_us;
  bool in_use;
} session_entry_t;

static session_entry_t s_sessions[MAX_SESSIONS];

static void bytes_to_hex(const uint8_t* bytes, size_t len, char* out_hex) {
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out_hex[i * 2] = digits[bytes[i] >> 4];
    out_hex[i * 2 + 1] = digits[bytes[i] & 0x0F];
  }
  out_hex[len * 2] = '\0';
}

static void compute_password_hash(const char* salt_hex, const char* password, char out_hash_hex[HASH_HEX_LEN + 1]) {
  char salted[SALT_HEX_LEN + 128];
  snprintf(salted, sizeof(salted), "%s%s", salt_hex, password);

  uint8_t hash[32];
  size_t hash_len = 0;
  psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)salted, strlen(salted), hash, sizeof(hash), &hash_len);
  bytes_to_hex(hash, hash_len, out_hash_hex);
}

static user_entry_t* find_user(const char* username) {
  for (size_t i = 0; i < MAX_USERS; i++) {
    if (s_users[i].in_use && strcmp(s_users[i].username, username) == 0) return &s_users[i];
  }
  return NULL;
}

static void save_users(void) {
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < MAX_USERS; i++) {
    if (!s_users[i].in_use) continue;
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "username", s_users[i].username);
    cJSON_AddStringToObject(obj, "salt", s_users[i].salt_hex);
    cJSON_AddStringToObject(obj, "hash", s_users[i].hash_hex);
    cJSON_AddNumberToObject(obj, "role", (int)s_users[i].role);
    cJSON_AddItemToArray(arr, obj);
  }
  char* text = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  if (!text) return;

  FILE* f = fopen(USERS_FILE, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  } else {
    ESP_LOGE(TAG, "Konnte %s nicht schreiben", USERS_FILE);
  }
  cJSON_free(text);
}

static void load_users(void) {
  FILE* f = fopen(USERS_FILE, "r");
  if (!f) return;

  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  cJSON* arr = cJSON_Parse(buf);
  if (!arr) return;

  int count = cJSON_GetArraySize(arr);
  for (int i = 0; i < count && s_user_count < MAX_USERS; i++) {
    cJSON* obj = cJSON_GetArrayItem(arr, i);
    cJSON* username = cJSON_GetObjectItem(obj, "username");
    cJSON* salt = cJSON_GetObjectItem(obj, "salt");
    cJSON* hash = cJSON_GetObjectItem(obj, "hash");
    cJSON* role = cJSON_GetObjectItem(obj, "role");
    if (!cJSON_IsString(username) || !cJSON_IsString(salt) || !cJSON_IsString(hash) || !cJSON_IsNumber(role)) continue;

    user_entry_t* u = &s_users[s_user_count++];
    strncpy(u->username, username->valuestring, USERNAME_CAP - 1);
    strncpy(u->salt_hex, salt->valuestring, SALT_HEX_LEN);
    strncpy(u->hash_hex, hash->valuestring, HASH_HEX_LEN);
    u->role = (user_role_t)role->valueint;
    u->in_use = true;
  }
  cJSON_Delete(arr);
}

static void create_default_admin(void) {
  ESP_LOGW(TAG,
           "Keine Benutzer gefunden - lege Default-Konto \"admin\"/\"admin\" (Rolle Admin) an. "
           "Passwort beim ersten Login unbedingt aendern!");
  user_manager_create("admin", "admin", USER_ROLE_ADMIN);
}

void user_manager_init(void) {
  psa_crypto_init();  // mehrfacher Aufruf ist laut PSA-Spezifikation sicher
  memset(s_users, 0, sizeof(s_users));
  memset(s_sessions, 0, sizeof(s_sessions));
  s_user_count = 0;

  load_users();
  if (s_user_count == 0) {
    create_default_admin();
  }
  ESP_LOGI(TAG, "UserManager gestartet (%u Konten)", (unsigned)s_user_count);
}

bool user_manager_authenticate(const char* username, const char* password, user_role_t* out_role) {
  user_entry_t* u = find_user(username);
  if (!u) return false;

  char computed[HASH_HEX_LEN + 1];
  compute_password_hash(u->salt_hex, password, computed);
  if (strcmp(computed, u->hash_hex) != 0) return false;

  *out_role = u->role;
  return true;
}

bool user_manager_validate_username(const char* username) {
  size_t len = strlen(username);
  if (len == 0 || len >= USERNAME_CAP) return false;
  for (size_t i = 0; i < len; i++) {
    char c = username[i];
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  }
  return true;
}

bool user_manager_validate_password(const char* password) {
  size_t len = strlen(password);
  if (len < 8) return false;

  bool has_upper = false, has_lower = false, has_digit = false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)password[i];
    if (isupper(c)) has_upper = true;
    else if (islower(c)) has_lower = true;
    else if (isdigit(c)) has_digit = true;
  }
  int classes = (has_upper ? 1 : 0) + (has_lower ? 1 : 0) + (has_digit ? 1 : 0);
  return classes >= 2;
}

bool user_manager_create(const char* username, const char* password, user_role_t role) {
  if (!user_manager_validate_username(username)) return false;
  if (!user_manager_validate_password(password)) return false;
  if (find_user(username)) return false;

  size_t slot = MAX_USERS;
  for (size_t i = 0; i < MAX_USERS; i++) {
    if (!s_users[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot == MAX_USERS) return false;  // voll

  uint8_t salt_bytes[SALT_HEX_LEN / 2];
  esp_fill_random(salt_bytes, sizeof(salt_bytes));

  user_entry_t* u = &s_users[slot];
  strncpy(u->username, username, USERNAME_CAP - 1);
  bytes_to_hex(salt_bytes, sizeof(salt_bytes), u->salt_hex);
  compute_password_hash(u->salt_hex, password, u->hash_hex);
  u->role = role;
  u->in_use = true;
  s_user_count++;

  save_users();
  return true;
}

bool user_manager_delete(const char* username) {
  user_entry_t* u = find_user(username);
  if (!u) return false;
  memset(u, 0, sizeof(*u));
  s_user_count--;
  save_users();
  return true;
}

bool user_manager_exists(const char* username) { return find_user(username) != NULL; }

size_t user_manager_count(void) { return s_user_count; }

bool user_manager_get_at(size_t index, char out_username[32], user_role_t* out_role) {
  size_t seen = 0;
  for (size_t i = 0; i < MAX_USERS; i++) {
    if (!s_users[i].in_use) continue;
    if (seen == index) {
      strncpy(out_username, s_users[i].username, USERNAME_CAP - 1);
      out_username[USERNAME_CAP - 1] = '\0';
      *out_role = s_users[i].role;
      return true;
    }
    seen++;
  }
  return false;
}

void user_manager_session_create(const char* username, user_role_t role, char out_token[33]) {
  uint8_t token_bytes[SESSION_TOKEN_HEX_LEN / 2];
  esp_fill_random(token_bytes, sizeof(token_bytes));
  bytes_to_hex(token_bytes, sizeof(token_bytes), out_token);

  size_t slot = 0;
  int64_t oldest = INT64_MAX;
  for (size_t i = 0; i < MAX_SESSIONS; i++) {
    if (!s_sessions[i].in_use) {
      slot = i;
      oldest = -1;
      break;
    }
    if (s_sessions[i].expires_at_us < oldest) {
      oldest = s_sessions[i].expires_at_us;
      slot = i;
    }
  }

  session_entry_t* s = &s_sessions[slot];
  strncpy(s->token_hex, out_token, SESSION_TOKEN_HEX_LEN);
  strncpy(s->username, username, USERNAME_CAP - 1);
  s->role = role;
  s->expires_at_us = esp_timer_get_time() + SESSION_TTL_US;
  s->in_use = true;
}

bool user_manager_session_validate(const char* token, char out_username[32], user_role_t* out_role) {
  int64_t now = esp_timer_get_time();
  for (size_t i = 0; i < MAX_SESSIONS; i++) {
    if (!s_sessions[i].in_use) continue;
    if (strcmp(s_sessions[i].token_hex, token) != 0) continue;
    if (s_sessions[i].expires_at_us < now) {
      s_sessions[i].in_use = false;
      return false;
    }
    strncpy(out_username, s_sessions[i].username, USERNAME_CAP - 1);
    *out_role = s_sessions[i].role;
    return true;
  }
  return false;
}

void user_manager_session_invalidate(const char* token) {
  for (size_t i = 0; i < MAX_SESSIONS; i++) {
    if (s_sessions[i].in_use && strcmp(s_sessions[i].token_hex, token) == 0) {
      s_sessions[i].in_use = false;
      return;
    }
  }
}

void user_manager_reset_to_default(void) {
  memset(s_users, 0, sizeof(s_users));
  memset(s_sessions, 0, sizeof(s_sessions));
  s_user_count = 0;
  remove(USERS_FILE);
  create_default_admin();
  ESP_LOGW(TAG, "Alle Benutzerkonten zurueckgesetzt - nur noch \"admin\"/\"admin\" vorhanden");
}
