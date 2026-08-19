#ifndef USERS_DATABASE_H
#define USERS_DATABASE_H

#include <stdint.h>
#include <pthread.h>

extern pthread_mutex_t g_db_lock;

int db_init(const char* db_path);
void db_close(void);
uint32_t db_create_user(const char* username, const char* password, const char* email, const char* birthday);
uint32_t db_verify_user(const char* username, const char* password);

#endif // USERS_DATABASE_H