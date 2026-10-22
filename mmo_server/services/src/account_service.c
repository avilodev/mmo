// Account rules for the login server, between the network code and storage.

#include "account_service.h"
#include "session.h"
#include "users_database.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(ACCOUNT_SESSION_KEY_SIZE == SESSION_KEY_LENGTH,
               "the session key size must match the session store");

account_start_result account_service_start(const char* users_db_path) {
	if(!db_init(users_db_path))
		return ACCOUNT_START_NO_DATABASE;

	if(!session_init()) {
		db_close();
		return ACCOUNT_START_NO_SESSIONS;
	}

	return ACCOUNT_START_OK;
}

void account_service_stop(void) {
	session_close();
	db_close();
}

int account_service_ready(void) {
	return session_is_ready();
}

uint32_t account_verify(const char* username, const char* password) {
	return db_verify_user(username, password);
}

uint32_t account_register(const char* username, const char* password,
                          const char* email, const char* birthday) {
	return db_create_user(username, password, email, birthday);
}

int account_issue_login_token(uint32_t account_id, char* out_token) {
	char* token = generate_session_key();
	int stored = token && auth_token_store(token, account_id);

	if(stored)
		memcpy(out_token, token, ACCOUNT_TOKEN_SIZE);

	free(token);
	return stored ? 1 : 0;
}

uint32_t account_redeem_login_token(const char* token) {
	return auth_token_consume(token);
}

int account_open_session(uint32_t account_id, const char* ip_address,
                         char* out_session_key) {
	return session_create(account_id, ip_address, out_session_key);
}
