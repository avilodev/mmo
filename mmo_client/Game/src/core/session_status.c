// The names of the statuses a player can show.

#include "core/session_status.h"

#include <ctype.h>
#include <string.h>

typedef struct {
	const char* word; // what is typed, lowercase
	const char* label; // what is shown
} status_name;

// One row per SESSION_STATUS_*, in value order.
static const status_name NAMES[SESSION_STATUS_COUNT] = {
	{ "online", "Online" },
	{ "afk", "AFK" },
	{ "dnd", "Do Not Disturb" },
	{ "offline", "Offline" },
};

const char* session_status_label(uint8_t status) {
	if(status >= SESSION_STATUS_COUNT)
		return "Unknown";

	return NAMES[status].label;
}

int session_status_parse(const char* word, size_t length, uint8_t* status) {
	for(int i = 0; i < SESSION_STATUS_COUNT; i++) {
		size_t n = strlen(NAMES[i].word);

		if(n != length)
			continue;

		int same = 1;

		for(size_t c = 0; c < n; c++) {
			if(tolower((unsigned char)word[c]) != NAMES[i].word[c])
				same = 0;
		}

		if(same) {
			*status = (uint8_t)i;

			return 1;
		}
	}

	return 0;
}
