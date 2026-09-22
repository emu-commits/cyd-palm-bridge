/* secrets.h -- simulator backstop. READ THE LIMIT BELOW BEFORE TRUSTING IT.
 *
 * This file is a no-op stub so that a source which includes "secrets.h" can
 * compile in the simulator without a real one existing.
 *
 * WHAT IT DOES NOT DO: it does not shield the build from a developer's real
 * firmware/main/secrets.h. It used to claim exactly that, on the grounds that
 * sim/include comes first in the include path -- and it was wrong for four
 * months. A QUOTED include (#include "secrets.h") is resolved relative to the
 * INCLUDING FILE'S OWN DIRECTORY before any -I path is searched. appcfg.c lives
 * in firmware/main, right next to the real secrets.h, so the real one won every
 * single time and this stub was never once opened. The simulator was built with
 * live Wi-Fi and Apple app-specific passwords inside it.
 *
 * The actual shield is -DSIM_NO_SECRETS (sim/Makefile), which makes appcfg.c
 * skip the include altogether, and `make -C sim nosecrets`, which fails if a
 * credential reaches the config from a seed.
 *
 * So this stub only helps a file that is NOT co-located with a real secrets.h
 * -- something under bridge/, or a test. That is a narrow but genuine job, and
 * it is the only one being claimed here now.
 */
#ifndef SIM_SECRETS_H
#define SIM_SECRETS_H
/* intentionally empty */
#endif
