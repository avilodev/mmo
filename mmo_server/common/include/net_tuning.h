#ifndef NET_TUNING_H
#define NET_TUNING_H

/** @file Let an operator override a reactor's worker pool at startup.
 *
 * The compiled defaults are derived from what each service contends for -- the
 * database connection pool, or memory bandwidth during password hashing -- and
 * those are properties of the machine and the deployment, not of this source.
 * The right number is therefore measurable but not knowable here, so it is a
 * knob with a sensible default rather than a constant.
 */

/** Read a worker count from the environment.
 *
 * @param variable  Name of the environment variable, e.g. "MMO_LOGIN_WORKERS".
 * @return The configured count, or 0 to mean "use the compiled default".
 */
int net_tuning_workers(const char* variable);

#endif // NET_TUNING_H
