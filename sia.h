#ifndef SIAH_H
#define SIAH_H

#include "miner.h"

extern void sia_gen_hash(const unsigned char *data, unsigned char *hash, unsigned int len);
extern void sia_regenhash(struct work *work);

#endif /* FRESHH_H */
