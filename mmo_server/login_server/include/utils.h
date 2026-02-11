#ifndef UTILS_H
#define UTILS_H

#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


int validate_username(const char* username);
int validate_password(const char* password);
int validate_email(const char* email);
int validate_birthday(const char* birthday);

#endif