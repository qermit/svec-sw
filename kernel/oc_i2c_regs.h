#ifndef __I2C_REGS_H
#define __I2C_REGS_H

#define I2C_REG_PRER_LO 0
#define I2C_REG_PRER_HI 4
#define I2C_REG_CTR 8
#define I2C_REG_TXR 0xc
#define I2C_REG_RXR 0xc
#define I2C_REG_CR 0x10
#define I2C_REG_SR 0x10

#define I2C_CTR_EN 0x80
#define I2C_CTR_IEN 0x40

#define I2C_CR_STA 0x80
#define I2C_CR_STO 0x40
#define I2C_CR_RD 0x20
#define I2C_CR_WR 0x10
#define I2C_CR_ACK 0x8
#define I2C_CR_IACK 0x1

#define I2C_SR_RXACK 0x80
#define I2C_SR_BUSY 0x40
#define I2C_SR_AL 0x20
#define I2C_SR_TIP 0x2
#define I2C_SR_IF 0x1

#endif
