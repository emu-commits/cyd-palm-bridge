/* monster_install.h -- the web flasher's optional Critters install. */
#ifndef MONSTER_INSTALL_H
#define MONSTER_INSTALL_H

/* After the card is mounted: copy a game the flasher wrote into the "game"
 * partition to /sdcard/games/monster/game.bin, once per new game. */
void monster_install(void);

#endif
