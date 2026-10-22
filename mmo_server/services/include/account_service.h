#ifndef ACCOUNT_SERVICE_H
#define ACCOUNT_SERVICE_H

// Account rules for the login server. Network code calls this, never storage.

#include <stdint.h>

// size of the single-use login token and of a session key, no terminator
#define ACCOUNT_TOKEN_SIZE 32
#define ACCOUNT_SESSION_KEY_SIZE 32

typedef enum {
	ACCOUNT_START_OK = 0,
	ACCOUNT_START_NO_DATABASE,
	ACCOUNT_START_NO_SESSIONS
} account_start_result;

// open the account database and the session store
account_start_result account_service_start(const char* users_db_path);

// close everything account_service_start opened
void account_service_stop(void);

// report whether the services behind the accounts are ready for requests
int account_service_ready(void);

// return the account id for good credentials, or 0
uint32_t account_verify(const char* username, const char* password);

// return the new account id, or 0 when the username is taken
uint32_t account_register(const char* username, const char* password,
                          const char* email, const char* birthday);

// write a single-use login token for the account, return 1 on success
int account_issue_login_token(uint32_t account_id, char* out_token);

// consume a login token and return its account id, or 0
uint32_t account_redeem_login_token(const char* token);

// open a session for the account and write its key, return 1 on success
int account_open_session(uint32_t account_id, const char* ip_address,
                         char* out_session_key);

#endif /* ACCOUNT_SERVICE_H */
