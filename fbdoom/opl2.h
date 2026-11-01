#ifndef OPL2_H
#define OPL2_H

#include <stdint.h>

typedef struct opl2_chip opl2_chip;

opl2_chip *OPL2_Create(void);

void OPL2_Reset(opl2_chip *chip, uint32_t rate);
void OPL2_WriteReg(opl2_chip *chip, uint16_t reg, uint8_t value);

// Writes n stereo (duplicated mono) frames.
void OPL2_GenerateStream(opl2_chip *chip, int16_t *buf, uint32_t n);

#endif
