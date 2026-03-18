#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

// Initialize the rate limiter. Call once at startup.
void rate_limiter_init(void);

// Returns 1 if the IP is currently blocked, 0 if allowed.
int  rate_limiter_check(const char* ip);

// Record a failed auth attempt from this IP.
// After RL_MAX_FAILS failures within RL_WINDOW_SECS, the IP is blocked
// for RL_BLOCK_SECS.
void rate_limiter_record_failure(const char* ip);

#endif // RATE_LIMITER_H
