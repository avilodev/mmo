#include "utils.h"

int validate_username(const char* username) {
    if(!username) return 0;
    if(strlen(username) < 3 || strlen(username) > 15) return 0;

    return 1;
}

int validate_password(const char* password) {
    if(!password) return 0;
    if(strlen(password) < 6 || strlen(password) > 20) return 0;

    return 1;
}

int validate_email(const char* email) {
    if (!email || strlen(email) < 3) return 0;
    
    const char* at = strchr(email, '@');
    if (!at || at == email) return 0;
    
    const char* dot = strchr(at, '.');
    if (!dot || dot[1] == '\0') return 0;
    
    if (strchr(at + 1, '@')) return 0;
    
    return 1;
}

int validate_birthday(const char* birthday) {
    if (!birthday) return 0;
    if(strlen(birthday) != 10) return 0;
    
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (birthday[i] != '-') return 0;
        } else {
            if (!isdigit(birthday[i])) return 0;
        }
    }
    
    // Extract and validate ranges
    int year = atoi(birthday);
    int month = atoi(birthday + 5);
    int day = atoi(birthday + 8);

    (void) year;
    
    if (month < 1 || month > 12) return 0;
    if (day < 1 || day > 31) return 0;
    
    // Days per month check
    int days_in_month[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (day > days_in_month[month - 1]) return 0;
    
    return 1;
}