/* secrets.h -- simulator shim. appcfg.c's secrets seeding is entirely #ifdef'd,
 * so an empty file means "no compile-time creds" -- the sim runs on
 * config_defaults() plus whatever /sdcard/config.ini provides. This file sits
 * ahead of firmware/main in the include order so a developer's REAL secrets.h
 * (gitignored, possibly present locally) can never leak into a sim build. */
#ifndef SIM_SECRETS_H
#define SIM_SECRETS_H
/* intentionally empty */
#endif
