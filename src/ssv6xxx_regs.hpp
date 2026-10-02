/*
 * SSV6X5X Register Definitions (C++14)
 * Derived from the Linux kernel driver (ssv6200_reg.h + ssv6200_aux.h)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>

/* ====================== Base Addresses ====================== */
constexpr uint32_t SYS_REG_BASE           = 0xC0000000U;
constexpr uint32_t WBOOT_REG_BASE         = 0xC0000100U;
constexpr uint32_t TU0_US_REG_BASE        = 0xC0000200U;
constexpr uint32_t TU1_US_REG_BASE        = 0xC0000210U;
constexpr uint32_t TU2_US_REG_BASE        = 0xC0000220U;
constexpr uint32_t TU3_US_REG_BASE        = 0xC0000230U;
constexpr uint32_t TM0_MS_REG_BASE        = 0xC0000240U;
constexpr uint32_t TM1_MS_REG_BASE        = 0xC0000250U;
constexpr uint32_t TM2_MS_REG_BASE        = 0xC0000260U;
constexpr uint32_t TM3_MS_REG_BASE        = 0xC0000270U;
constexpr uint32_t INT_REG_BASE           = 0xC0000E00U;
constexpr uint32_t HCI_REG_BASE           = 0xC1000000U;
constexpr uint32_t CO_REG_BASE            = 0xC2000000U;
constexpr uint32_t MRX_REG_BASE           = 0xC6000000U;
constexpr uint32_t AMPDU_REG_BASE         = 0xC6001000U;
constexpr uint32_t MT_REG_CSR_BASE        = 0xC6002000U;
constexpr uint32_t TXQ0_MT_Q_REG_CSR_BASE = 0xC6002100U;
constexpr uint32_t TXQ1_MT_Q_REG_CSR_BASE = 0xC6002200U;
constexpr uint32_t TXQ2_MT_Q_REG_CSR_BASE = 0xC6002300U;
constexpr uint32_t TXQ3_MT_Q_REG_CSR_BASE = 0xC6002400U;
constexpr uint32_t TXQ4_MT_Q_REG_CSR_BASE = 0xC6002500U;
constexpr uint32_t HIF_INFO_BASE          = 0xCA000000U;
constexpr uint32_t PHY_RATE_INFO_BASE     = 0xCA000200U;
constexpr uint32_t MAC_GLB_SET_BASE       = 0xCA000300U;
constexpr uint32_t BTCX_REG_BASE          = 0xCA000400U;
constexpr uint32_t MIB_REG_BASE           = 0xCA000800U;
constexpr uint32_t MB_REG_BASE            = 0xCD000000U;
constexpr uint32_t ID_MNG_REG_BASE        = 0xCD010000U;
constexpr uint32_t CSR_PHY_BASE           = 0xCE000000U;
constexpr uint32_t CSR_RF_BASE            = 0xCE010000U;
constexpr uint32_t MMU_REG_BASE           = 0xCF000000U;
constexpr uint32_t CSR_TU_PHY_BASE        = 0xCCB0E000U;

/* ====================== System Registers ====================== */
constexpr uint32_t ADR_BRG_SW_RST            = SYS_REG_BASE + 0x00;
constexpr uint32_t ADR_BOOT                  = SYS_REG_BASE + 0x04;
constexpr uint32_t ADR_CHIP_ID_0             = SYS_REG_BASE + 0x08;
constexpr uint32_t ADR_CHIP_ID_1             = SYS_REG_BASE + 0x0C;
constexpr uint32_t ADR_CHIP_ID_2             = SYS_REG_BASE + 0x10;
constexpr uint32_t ADR_CHIP_ID_3             = SYS_REG_BASE + 0x14;
constexpr uint32_t ADR_CLOCK_SELECTION       = SYS_REG_BASE + 0x18;
constexpr uint32_t ADR_PLATFORM_CLOCK_ENABLE = SYS_REG_BASE + 0x1C;
constexpr uint32_t ADR_SYS_CSR_CLOCK_EN      = SYS_REG_BASE + 0x20;
constexpr uint32_t ADR_MANUAL_RESET_N        = SYS_REG_BASE + 0xB4;
constexpr uint32_t ADR_USB20_HOST_SEL        = SYS_REG_BASE + 0xF0;
constexpr uint32_t ADR_N10CFG_DEF_IVB        = SYS_REG_BASE + 0xE8;
constexpr uint32_t ADR_N10_DBG2              = SYS_REG_BASE + 0xEC;
constexpr uint32_t ADR_PRESCALER_USTIMER     = SYS_REG_BASE + 0xA0;
constexpr uint32_t ADR_TEST_MODE             = SYS_REG_BASE + 0xB0;

/* ====================== SYSCTRL Registers ====================== */
constexpr uint32_t CSR_ALLON_BASE        = 0xC0008000U;
constexpr uint32_t ADR_SYSCTRL_STATUS    = CSR_ALLON_BASE + 0x00;
constexpr uint32_t ADR_SYSCTRL_COMMAND   = CSR_ALLON_BASE + 0x04;
constexpr uint32_t ADR_POWER_ON_OFF_CTRL = CSR_ALLON_BASE + 0x08;

/* ====================== USB ACC Control Registers ====================== */
/* ssv6006C_reg.h: USB20_REG_BASE = 0x70000000. */
constexpr uint32_t USB20_REG_BASE         = 0x70000000U;
constexpr uint32_t ADR_USB_ACC_CTRL_REG_0 = USB20_REG_BASE + 0x41AC;
constexpr uint32_t ADR_USB_ACC_CTRL_REG_1 = USB20_REG_BASE + 0x41B8;

/* ====================== Bridge/Reset Bit Fields ====================== */
constexpr uint32_t MAC_SW_RST_MSK = 0x00000002U;
constexpr int MAC_SW_RST_SFT      = 1;
constexpr uint32_t MCU_SW_RST_MSK = 0x00000004U;
constexpr int MCU_SW_RST_SFT      = 2;

/* ====================== WDOG Registers ====================== */
constexpr uint32_t ADR_MCU_WDOG_REG = SYS_REG_BASE + 0x0280;
constexpr uint32_t ADR_SYS_WDOG_REG = SYS_REG_BASE + 0x0284;

/* ====================== Boot Info ====================== */
constexpr uint32_t ADR_BOOT_INFO = WBOOT_REG_BASE + 0x00;

/* ====================== Timer Registers ====================== */
constexpr uint32_t ADR_TU0_MS_TIMER    = TU0_US_REG_BASE + 0x00;
constexpr uint32_t ADR_TU0_CUR_US_TIME = TU0_US_REG_BASE + 0x04;

/* ====================== Interrupt Registers ====================== */
constexpr uint32_t ADR_INT_MASK            = INT_REG_BASE + 0x00;
constexpr uint32_t ADR_INT_MODE            = INT_REG_BASE + 0x04;
constexpr uint32_t ADR_INT_IRQ_STS         = INT_REG_BASE + 0x08;
constexpr uint32_t ADR_INT_FIQ_STS         = INT_REG_BASE + 0x0C;
constexpr uint32_t ADR_INT_IRQ_RAW         = INT_REG_BASE + 0x10;
constexpr uint32_t ADR_INT_FIQ_RAW         = INT_REG_BASE + 0x14;
constexpr uint32_t ADR_INT_SDIO_IPC        = INT_REG_BASE + 0x38;
constexpr uint32_t ADR_INT_SDIO_MASK       = INT_REG_BASE + 0x3C;
constexpr uint32_t ADR_INT_SDIO_IRQ_STS    = INT_REG_BASE + 0x40;
constexpr uint32_t ADR_MASK_TYPMCU_INT_MAP = INT_REG_BASE + 0xC0;

/* ====================== HCI Registers ====================== */
constexpr uint32_t ADR_CONTROL                   = HCI_REG_BASE + 0x00;
constexpr uint32_t ADR_SDIO_WAKE_MODE            = HCI_REG_BASE + 0x04;
constexpr uint32_t ADR_TX_FLOW_0                 = HCI_REG_BASE + 0x08;
constexpr uint32_t ADR_TX_FLOW_1                 = HCI_REG_BASE + 0x0C;
constexpr uint32_t ADR_THREASHOLD                = HCI_REG_BASE + 0x18;
constexpr uint32_t ADR_TXFID_INCREASE            = HCI_REG_BASE + 0x20;
constexpr uint32_t ADR_GLOBAL_SEQUENCE           = HCI_REG_BASE + 0x28;
constexpr uint32_t ADR_HCI_TX_RX_INFO_SIZE       = HCI_REG_BASE + 0x30;
constexpr uint32_t ADR_HCI_TX_INFO_CLEAR         = HCI_REG_BASE + 0x34;
constexpr uint32_t ADR_HCI_FORCE_PRE_BULK_IN     = HCI_REG_BASE + 0xA0;
constexpr uint32_t ADR_HCI_TRX_MODE              = HCI_REG_BASE + 0x04;
constexpr uint32_t ADR_FORCE_RX_AGGREGATION_MODE = HCI_REG_BASE + 0x168;
constexpr uint32_t ADR_RX_PACKET_LENGTH_STATUS   = HCI_REG_BASE + 0x14;
constexpr uint32_t ADR_TX_ETHER_TYPE_0           = HCI_REG_BASE + 0x50;
constexpr uint32_t ADR_TX_ETHER_TYPE_1           = HCI_REG_BASE + 0x54;
constexpr uint32_t ADR_RX_ETHER_TYPE_0           = HCI_REG_BASE + 0x60;
constexpr uint32_t ADR_RX_ETHER_TYPE_1           = HCI_REG_BASE + 0x64;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_0     = HCI_REG_BASE + 0x70;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_1     = HCI_REG_BASE + 0x74;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_2     = HCI_REG_BASE + 0x78;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_3     = HCI_REG_BASE + 0x7C;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_4     = HCI_REG_BASE + 0x80;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_5     = HCI_REG_BASE + 0x84;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_6     = HCI_REG_BASE + 0x88;
constexpr uint32_t ADR_PACKET_COUNTER_INFO_7     = HCI_REG_BASE + 0x8C;

/* HCI TX/RX Info Size bit fields */
constexpr int TX_PBOFFSET_SFT      = 0;
constexpr int TX_INFO_SIZE_SFT     = 8;
constexpr int RX_INFO_SIZE_SFT     = 16;
constexpr int RX_LAST_PHY_SIZE_SFT = 24;

/* HCI_TRX_MODE bit fields */
constexpr uint32_t HCI_TX_AGG_EN_MSK = 0x00000001U;
constexpr int HCI_TX_AGG_EN_SFT      = 0;
constexpr uint32_t HCI_RX_EN_MSK     = 0x00000002U;
constexpr int HCI_RX_EN_SFT          = 1;
constexpr uint32_t HCI_RX_FORM_1_MSK = 0x40000000U;
constexpr int HCI_RX_FORM_1_SFT      = 30;
constexpr uint32_t HCI_RX_FORM_0_MSK = 0x80000000U;
constexpr int HCI_RX_FORM_0_SFT      = 31;

/* FORCE_RX_AGGREGATION_MODE bit fields */
constexpr uint32_t RX_AGG_CNT_MSK                = 0x0000000FU;
constexpr int RX_AGG_CNT_SFT                     = 0;
constexpr uint32_t RX_AGG_METHOD_3_MSK           = 0x00000080U;
constexpr int RX_AGG_METHOD_3_SFT                = 7;
constexpr uint32_t RX_AGG_TIMER_RELOAD_VALUE_MSK = 0xFFFF0000U;
constexpr int RX_AGG_TIMER_RELOAD_VALUE_SFT      = 16;

/* HCI_FORCE_PRE_BULK_IN bit fields */
constexpr uint32_t HCI_BULK_IN_HOST_SIZE_MSK = 0x0001FFFFU;

/* ====================== MAC Global Setting Registers ====================== */
constexpr uint32_t ADR_MAC_MODE           = MAC_GLB_SET_BASE + 0x00;
constexpr uint32_t ADR_ALL_SOFTWARE_RESET = MAC_GLB_SET_BASE + 0x04;
constexpr uint32_t ADR_ENG_SOFTWARE_RESET = MAC_GLB_SET_BASE + 0x08;
constexpr uint32_t ADR_CSR_SOFTWARE_RESET = MAC_GLB_SET_BASE + 0x0C;
constexpr uint32_t ADR_MAC_CLOCK_ENABLE   = MAC_GLB_SET_BASE + 0x10;
constexpr uint32_t ADR_MAC_ENG_CLK_ENABLE = MAC_GLB_SET_BASE + 0x14;
constexpr uint32_t ADR_MAC_CSR_CLK_ENABLE = MAC_GLB_SET_BASE + 0x18;
constexpr uint32_t ADR_GLBLE_SET          = MAC_GLB_SET_BASE + 0x1C;
constexpr uint32_t ADR_REASON_TRAP0       = MAC_GLB_SET_BASE + 0x20;
constexpr uint32_t ADR_REASON_TRAP1       = MAC_GLB_SET_BASE + 0x24;
constexpr uint32_t ADR_BSSID_0            = MAC_GLB_SET_BASE + 0x28;
constexpr uint32_t ADR_BSSID_1            = MAC_GLB_SET_BASE + 0x2C;
constexpr uint32_t ADR_STA_MAC_0          = MAC_GLB_SET_BASE + 0x30;
constexpr uint32_t ADR_STA_MAC_1          = MAC_GLB_SET_BASE + 0x34;
constexpr uint32_t ADR_SCRT_SET           = MAC_GLB_SET_BASE + 0x38;

/* GLBLE_SET bit fields */
constexpr uint32_t OP_MODE_MSK       = 0x00000003U;
constexpr int OP_MODE_SFT            = 0;
constexpr uint32_t SNIFFER_MODE_MSK  = 0x00010000U;
constexpr int SNIFFER_MODE_SFT       = 16;
constexpr uint32_t AMPDU_SNIFFER_MSK = 0x00200000U;
constexpr int AMPDU_SNIFFER_SFT      = 21;
constexpr int DUP_FLT_SFT            = 2;
constexpr uint32_t DUP_FLT_MSK       = 0x00000004U;
constexpr int TX_PKT_RSVD_SFT        = 4;
constexpr uint32_t TX_PKT_RSVD_MSK   = 0x000000F0U;
constexpr int PB_OFFSET_SFT          = 8;
constexpr uint32_t PB_OFFSET_MSK     = 0x00000F00U;

/* SCRT_SET bit fields */
constexpr uint32_t SCRT_PKT_ID_MSK = 0x00001FC0U;
constexpr int SCRT_PKT_ID_SFT      = 6;

/* ====================== Co-Processor Registers ====================== */
constexpr uint32_t ADR_CS_START_ADDR = CO_REG_BASE + 0x00;
constexpr uint32_t ADR_CS_CHECK_SUM  = CO_REG_BASE + 0x14;

/* ====================== MAC RX Registers ====================== */
constexpr uint32_t ADR_MRX_MCAST_TB0_0     = MRX_REG_BASE + 0x00;
constexpr uint32_t ADR_MRX_FLT_TB0         = MRX_REG_BASE + 0x70;
constexpr uint32_t ADR_MRX_FLT_TB1         = MRX_REG_BASE + 0x74;
constexpr uint32_t ADR_MRX_FLT_TB2         = MRX_REG_BASE + 0x78;
constexpr uint32_t ADR_MRX_FLT_TB3         = MRX_REG_BASE + 0x7C;
constexpr uint32_t ADR_MRX_FLT_TB4         = MRX_REG_BASE + 0x80;
constexpr uint32_t ADR_MRX_FLT_TB5         = MRX_REG_BASE + 0x84;
constexpr uint32_t ADR_MRX_FLT_TB6         = MRX_REG_BASE + 0x88;
constexpr uint32_t ADR_MRX_FLT_TB7         = MRX_REG_BASE + 0x8C;
constexpr uint32_t ADR_MRX_FLT_TB8         = MRX_REG_BASE + 0x90;
constexpr uint32_t ADR_MRX_FLT_TB9         = MRX_REG_BASE + 0x94;
constexpr uint32_t ADR_MRX_FLT_TB10        = MRX_REG_BASE + 0x98;
constexpr uint32_t ADR_MRX_FLT_TB11        = MRX_REG_BASE + 0x9C;
constexpr uint32_t ADR_MRX_FLT_TB12        = MRX_REG_BASE + 0xA0;
constexpr uint32_t ADR_MRX_FLT_TB13        = MRX_REG_BASE + 0xA4;
constexpr uint32_t ADR_MRX_FLT_TB14        = MRX_REG_BASE + 0xA8;
constexpr uint32_t ADR_MRX_FLT_TB15        = MRX_REG_BASE + 0xAC;
constexpr uint32_t ADR_MRX_FLT_EN0         = MRX_REG_BASE + 0xB0;
constexpr uint32_t ADR_MRX_FLT_EN1         = MRX_REG_BASE + 0xB4;
constexpr uint32_t ADR_MRX_FLT_EN2         = MRX_REG_BASE + 0xB8;
constexpr uint32_t ADR_MRX_FLT_EN3         = MRX_REG_BASE + 0xBC;
constexpr uint32_t ADR_MRX_FLT_EN4         = MRX_REG_BASE + 0xC0;
constexpr uint32_t ADR_MRX_FLT_EN5         = MRX_REG_BASE + 0xC4;
constexpr uint32_t ADR_MRX_FLT_EN6         = MRX_REG_BASE + 0xC8;
constexpr uint32_t ADR_MRX_FLT_EN7         = MRX_REG_BASE + 0xCC;
constexpr uint32_t ADR_MRX_FLT_EN8         = MRX_REG_BASE + 0xD0;
constexpr uint32_t ADR_MRX_LEN_FLT         = MRX_REG_BASE + 0xD4;
constexpr uint32_t ADR_RX_FLOW_DATA        = MRX_REG_BASE + 0xE0;
constexpr uint32_t ADR_RX_FLOW_MNG         = MRX_REG_BASE + 0xE4;
constexpr uint32_t ADR_RX_FLOW_CTRL        = MRX_REG_BASE + 0xE8;
constexpr uint32_t ADR_RX_TIME_STAMP_CFG   = MRX_REG_BASE + 0xEC;
constexpr uint32_t ADR_MRX_WATCH_DOG       = MRX_REG_BASE + 0x11C;
constexpr uint32_t ADR_ACK_GEN_EN          = MRX_REG_BASE + 0x120;
constexpr uint32_t ADR_ACK_GEN_PARA        = MRX_REG_BASE + 0x124;
constexpr uint32_t ADR_ACK_GEN_RA_0        = MRX_REG_BASE + 0x128;
constexpr uint32_t ADR_ACK_GEN_RA_1        = MRX_REG_BASE + 0x12C;
constexpr uint32_t ADR_TRAP_HW_ID          = MRX_REG_BASE + 0x134;
constexpr uint32_t ADR_ID_IN_USE           = MRX_REG_BASE + 0x138;
constexpr uint32_t ADR_MRX_ERR             = MRX_REG_BASE + 0x13C;
constexpr uint32_t ADR_HDR_ADDR_SEL        = MRX_REG_BASE + 0x190;
constexpr uint32_t ADR_FRAME_TYPE_CNTR_SET = MRX_REG_BASE + 0x194;

/* RX Flow Engine IDs */
constexpr uint8_t M_ENG_CPU         = 0x00;
constexpr uint8_t M_ENG_HWHCI       = 0x01;
constexpr uint8_t M_ENG_EMPTY       = 0x02;
constexpr uint8_t M_ENG_ENCRYPT     = 0x03;
constexpr uint8_t M_ENG_MACRX       = 0x04;
constexpr uint8_t M_ENG_MIC         = 0x05;
constexpr uint8_t M_ENG_ENCRYPT_SEC = 0x0B;
constexpr uint8_t M_ENG_MIC_SEC     = 0x0C;
constexpr uint8_t M_ENG_TRASH_CAN   = 0x0F;

/* RX Flow routing helpers */
constexpr uint32_t RX_HCI = M_ENG_MACRX | (M_ENG_HWHCI << 4);
constexpr uint32_t RX_CIPHER_HCI =
    M_ENG_MACRX | (M_ENG_ENCRYPT_SEC << 4) | (M_ENG_HWHCI << 8);
constexpr uint32_t RX_CPU_HCI =
    M_ENG_MACRX | (M_ENG_CPU << 4) | (M_ENG_HWHCI << 8);
constexpr uint32_t RX_TRASH = M_ENG_MACRX | (M_ENG_TRASH_CAN << 4);

/* RX Timestamp config */
constexpr int MRX_STP_OFST_SFT = 8;

/* ====================== AMPDU Registers ====================== */
constexpr uint32_t ADR_PHY_INFO  = AMPDU_REG_BASE + 0x00;
constexpr uint32_t ADR_AMPDU_SIG = AMPDU_REG_BASE + 0x04;

/* ====================== MAC TX Registers ====================== */
constexpr uint32_t ADR_MTX_INT_STS       = MT_REG_CSR_BASE + 0x00;
constexpr uint32_t ADR_MTX_INT_EN        = MT_REG_CSR_BASE + 0x04;
constexpr uint32_t ADR_MTX_MISC_EN       = MT_REG_CSR_BASE + 0x08;
constexpr uint32_t ADR_MTX_BCN_INT_STS   = MT_REG_CSR_BASE + 0xA0;
constexpr uint32_t ADR_MTX_BCN_EN_INT    = MT_REG_CSR_BASE + 0xA4;
constexpr uint32_t ADR_MTX_BCN_EN_MISC   = MT_REG_CSR_BASE + 0xA8;
constexpr uint32_t ADR_MTX_BCN_MISC      = MT_REG_CSR_BASE + 0xAC;
constexpr uint32_t ADR_MTX_BCN_PRD       = MT_REG_CSR_BASE + 0xB0;
constexpr uint32_t ADR_MTX_BCN_TSF_L     = MT_REG_CSR_BASE + 0xB4;
constexpr uint32_t ADR_MTX_BCN_TSF_U     = MT_REG_CSR_BASE + 0xB8;
constexpr uint32_t ADR_MTX_STATUS        = MT_REG_CSR_BASE + 0xCC;
constexpr uint32_t ADR_MTX_DBG_CTRL      = MT_REG_CSR_BASE + 0xD0;
constexpr uint32_t ADR_MTX_NAV           = MT_REG_CSR_BASE + 0xF0;
constexpr uint32_t ADR_MTX_TIME_IFS      = MT_REG_CSR_BASE + 0xC4;
constexpr uint32_t ADR_MTX_TIME_FINETUNE = MT_REG_CSR_BASE + 0xC8;
constexpr uint32_t ADR_DIGITAL_ADD_ON_0  = 0xCCB0A800U;

/* MTX_BCN_EN_MISC bit fields */
constexpr uint32_t MTX_TSF_TIMER_EN_MSK = 0x00000020U;
constexpr int MTX_TSF_TIMER_EN_SFT      = 5;

/* MTX_MISC_EN bit fields */
constexpr uint32_t MTX_BLOCKTX_IGNORE_TOMAC_CCA_CS_MSK           = 0x00002000U;
constexpr int MTX_BLOCKTX_IGNORE_TOMAC_CCA_CS_SFT                = 13;
constexpr uint32_t MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_SECONDARY_MSK = 0x00004000U;
constexpr int MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_SECONDARY_SFT      = 14;
constexpr uint32_t MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_PRIMARY_MSK   = 0x00008000U;
constexpr int MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_PRIMARY_SFT        = 15;

/* PHY common sys reg bit fields */
constexpr uint32_t RG_PRIMARY_CH_SIDE_MSK      = 0x00004000U;
constexpr int RG_PRIMARY_CH_SIDE_SFT           = 14;
constexpr uint32_t RG_SYSTEM_BW_MSK            = 0x00008000U;
constexpr int RG_SYSTEM_BW_SFT                 = 15;
constexpr uint32_t RG_40M_MODE_MSK             = 0x01000000U;
constexpr int RG_40M_MODE_SFT                  = 24;
constexpr uint32_t RG_LO_UP_CH_MSK             = 0x10000000U;
constexpr int RG_LO_UP_CH_SFT                  = 28;
constexpr uint32_t SIFS_MSK                    = 0x001F0000U;
constexpr int SIFS_SFT                         = 16;
constexpr uint32_t SIGEXT_MSK                  = 0x0F000000U;
constexpr int SIGEXT_SFT                       = 24;
constexpr uint32_t MTX_HALT_MNG_UNTIL_DTIM_MSK = 0x00000040U;

/* TX Queue Registers (5 queues: BK=0, BE=1, VI=2, VO=3, MNG=4) */
constexpr uint32_t ADR_TXQ0_MTX_Q_MISC_EN = TXQ0_MT_Q_REG_CSR_BASE + 0x00;
constexpr uint32_t ADR_TXQ0_MTX_Q_AIFSN   = TXQ0_MT_Q_REG_CSR_BASE + 0x04;
constexpr uint32_t ADR_TXQ1_MTX_Q_AIFSN   = TXQ1_MT_Q_REG_CSR_BASE + 0x04;
constexpr uint32_t ADR_TXQ2_MTX_Q_AIFSN   = TXQ2_MT_Q_REG_CSR_BASE + 0x04;
constexpr uint32_t ADR_TXQ3_MTX_Q_AIFSN   = TXQ3_MT_Q_REG_CSR_BASE + 0x04;
constexpr uint32_t ADR_TXQ4_MTX_Q_AIFSN   = TXQ4_MT_Q_REG_CSR_BASE + 0x04;

/* ====================== HIF Info ====================== */
constexpr uint32_t ADR_WSID0 = HIF_INFO_BASE + 0x00;
constexpr uint32_t ADR_WSID1 = HIF_INFO_BASE + 0x50;

/* ====================== OPMODE Values ====================== */
constexpr uint8_t SSV6XXX_OPMODE_STA  = 0;
constexpr uint8_t SSV6XXX_OPMODE_AP   = 1;
constexpr uint8_t SSV6XXX_OPMODE_IBSS = 2;
constexpr uint8_t SSV6XXX_OPMODE_WDS  = 3;

/* ====================== PHY Rate Info ====================== */
constexpr uint32_t ADR_IC_TIME_TAG_0 = PHY_RATE_INFO_BASE + 0xAC;
constexpr uint32_t ADR_IC_TIME_TAG_1 = PHY_RATE_INFO_BASE + 0xB0;

/* ====================== ID Management ====================== */
constexpr uint32_t ADR_WR_ALC              = ID_MNG_REG_BASE + 0x00;
constexpr uint32_t ADR_GETID               = ID_MNG_REG_BASE + 0x00;
constexpr uint32_t ADR_CH_STA_PRI          = ID_MNG_REG_BASE + 0x04;
constexpr uint32_t ADR_RD_ID0              = ID_MNG_REG_BASE + 0x08;
constexpr uint32_t ADR_RD_ID1              = ID_MNG_REG_BASE + 0x0C;
constexpr uint32_t ADR_IMD_CFG             = ID_MNG_REG_BASE + 0x10;
constexpr uint32_t ADR_IMD_STA             = ID_MNG_REG_BASE + 0x14;
constexpr uint32_t ADR_ALC_STA             = ID_MNG_REG_BASE + 0x18;
constexpr uint32_t ADR_TRX_ID_COUNT        = ID_MNG_REG_BASE + 0x1C;
constexpr uint32_t ADR_TRX_ID_THRESHOLD    = ID_MNG_REG_BASE + 0x20;
constexpr uint32_t ADR_TX_ID0              = ID_MNG_REG_BASE + 0x24;
constexpr uint32_t ADR_TX_ID1              = ID_MNG_REG_BASE + 0x28;
constexpr uint32_t ADR_RX_ID0              = ID_MNG_REG_BASE + 0x2C;
constexpr uint32_t ADR_RX_ID1              = ID_MNG_REG_BASE + 0x30;
constexpr uint32_t ADR_RTN_STA             = ID_MNG_REG_BASE + 0x34;
constexpr uint32_t ADR_ID_LEN_THREADSHOLD1 = ID_MNG_REG_BASE + 0x38;
constexpr uint32_t ADR_ID_LEN_THREADSHOLD2 = ID_MNG_REG_BASE + 0x3C;
constexpr uint32_t ADR_CH_ARB_PRI          = ID_MNG_REG_BASE + 0x40;
constexpr uint32_t ADR_TX_ID_REMAIN_STATUS = ID_MNG_REG_BASE + 0x44;
constexpr uint32_t ADR_ID_INFO_STA         = ID_MNG_REG_BASE + 0x48;
constexpr uint32_t ADR_TX_LIMIT_INTR       = ID_MNG_REG_BASE + 0x4C;
constexpr uint32_t ADR_TX_ID_ALL_INFO      = ID_MNG_REG_BASE + 0x50;
constexpr uint32_t ADR_ALC_ID_INFO         = ID_MNG_REG_BASE + 0x7C;
constexpr uint32_t ADR_ALC_ID_INF1         = ID_MNG_REG_BASE + 0x80;

/* ID threshold bit fields */
constexpr uint32_t TX_ID_THOLD_MSK = 0x000000FFU;
constexpr int TX_ID_THOLD_SFT      = 0;
constexpr uint32_t RX_ID_THOLD_MSK = 0x0000FF00U;
constexpr int RX_ID_THOLD_SFT      = 8;

/* ID length threshold bit fields */
constexpr uint32_t ID_TX_LEN_THOLD_MSK = 0x00001FF0U;
constexpr int ID_TX_LEN_THOLD_SFT      = 4;
constexpr uint32_t ID_RX_LEN_THOLD_MSK = 0x003FE000U;
constexpr int ID_RX_LEN_THOLD_SFT      = 13;

/* ====================== Mailbox Registers ====================== */
constexpr uint32_t ADR_MB_CPU_INT         = MB_REG_BASE + 0x04;
constexpr uint32_t ADR_CH0_TRIG_0         = MB_REG_BASE + 0x10;
constexpr uint32_t ADR_CH0_PRI_TRIG       = MB_REG_BASE + 0x14;
constexpr uint32_t ADR_MCU_STATUS         = MB_REG_BASE + 0x18;
constexpr uint32_t ADR_RD_IN_FFCNT1       = MB_REG_BASE + 0x1C;
constexpr uint32_t ADR_RD_IN_FFCNT2       = MB_REG_BASE + 0x20;
constexpr uint32_t ADR_RD_FFIN_FULL       = MB_REG_BASE + 0x24;
constexpr uint32_t ADR_MBOX_HALT_CFG      = MB_REG_BASE + 0x2C;
constexpr uint32_t ADR_MB_OUT_QUEUE_CFG   = MB_REG_BASE + 0x40;
constexpr uint32_t ADR_MB_OUT_QUEUE_FLUSH = MB_REG_BASE + 0x44;
constexpr uint32_t ADR_RD_FFOUT_CNT1      = MB_REG_BASE + 0x48;
constexpr uint32_t ADR_RD_FFOUT_CNT2      = MB_REG_BASE + 0x4C;
constexpr uint32_t ADR_RD_FFOUT_CNT3      = MB_REG_BASE + 0x50;
constexpr uint32_t ADR_RD_FFOUT_FULL      = MB_REG_BASE + 0x54;
constexpr uint32_t ADR_MB_THRESHOLD6      = MB_REG_BASE + 0x6C;
constexpr uint32_t ADR_MB_THRESHOLD7      = MB_REG_BASE + 0x70;
constexpr uint32_t ADR_MB_THRESHOLD8      = MB_REG_BASE + 0x74;
constexpr uint32_t ADR_MB_THRESHOLD9      = MB_REG_BASE + 0x78;
constexpr uint32_t ADR_MB_THRESHOLD10     = MB_REG_BASE + 0x7C;
constexpr uint32_t ADR_MB_TRASH_CFG       = MB_REG_BASE + 0x80;
constexpr uint32_t ADR_MB_IN_FF_FLUSH     = MB_REG_BASE + 0x84;

/* ====================== MMU Registers ====================== */
constexpr uint32_t ADR_MMU_CTRL             = MMU_REG_BASE + 0x00;
constexpr uint32_t ADR_MMU_INT              = MMU_REG_BASE + 0x04;
constexpr uint32_t ADR_MMU_PAGE_CNT         = MMU_REG_BASE + 0x08;
constexpr uint32_t ADR_MMU_TABLE_ADDR       = MMU_REG_BASE + 0x0C;
constexpr uint32_t ADR_MMU_ACCESS_VIOLATION = MMU_REG_BASE + 0x10;
constexpr uint32_t ADR_MMU_DEBUG            = MMU_REG_BASE + 0x14;
constexpr uint32_t ADR_MMU_CTRL_DATA        = MMU_REG_BASE + 0x18;

constexpr uint32_t MMU_SHARE_MCU_MSK = 0x00FF0000U;
constexpr int MMU_SHARE_MCU_SFT      = 16;

/* ====================== CBR (RF/Analog) Registers ====================== */
constexpr uint32_t CBR_A_REG_BASE = 0xCB000000;
/* TurismoC RG_MODE lives in ADR_TU_MODE_REG (0xCCB0A400), bits [10:8].
 * The old 0xCB110000/bits[13:12] definition belongs to another RF block and
 * silently left the receiver disabled on SSV6006C/SSV6256P. */
constexpr uint32_t ADR_CBR_HARD_WIRE_PIN = 0xCCB0A400;
constexpr uint32_t ADR_CBR_MANUAL_ENABLE = CBR_A_REG_BASE + 0x00110004;
constexpr uint32_t ADR_CBR_SYN_1         = CBR_A_REG_BASE + 0x0011003C;
constexpr uint32_t ADR_CBR_SYN_2         = CBR_A_REG_BASE + 0x00110040;
constexpr uint32_t ADR_CBR_SX_LCK_1      = CBR_A_REG_BASE + 0x00110050;
constexpr uint32_t ADR_CBR_SX_LCK_2      = CBR_A_REG_BASE + 0x00110054;
constexpr uint32_t ADR_CBR_RO_FLAGS_1    = CBR_A_REG_BASE + 0x00110094;
constexpr uint32_t ADR_CBR_RO_FLAGS_2    = CBR_A_REG_BASE + 0x00110098;
constexpr uint32_t ADR_CBR_RX_TX_FSM     = CBR_A_REG_BASE + 0x0011002C;

/* ====================== TurismoC RF Calibration Registers
 * ====================== */
constexpr uint32_t CSR_TU_RF_BASE               = 0xCCB0A000;
constexpr uint32_t ADR_TU_MODE_REG              = CSR_TU_RF_BASE + 0x00000400;
constexpr uint32_t ADR_TU_2G_TRX_MANUAL         = CSR_TU_RF_BASE + 0x00000404;
constexpr uint32_t ADR_TU_2G_CH_TABLE           = CSR_TU_RF_BASE + 0x00000464;
constexpr uint32_t ADR_TU_CAL_GAIN0             = CSR_TU_RF_BASE + 0x00000538;
constexpr uint32_t ADR_TU_5G_CH_TABLE           = CSR_TU_RF_BASE + 0x00000580;
constexpr uint32_t ADR_TU_5G_DCOC_IDAC_BASE     = CSR_TU_RF_BASE + 0x000005A8;
constexpr uint32_t ADR_TU_5G_TX_DAC             = CSR_TU_RF_BASE + 0x00000578;
constexpr uint32_t ADR_TU_5G_CAL_TIMER_GAIN     = CSR_TU_RF_BASE + 0x00000608;
constexpr uint32_t ADR_TU_5G_CAL_GAIN1          = CSR_TU_RF_BASE + 0x0000060C;
constexpr uint32_t ADR_TU_5G_RO_FLAGS_1         = CSR_TU_RF_BASE + 0x00000618;
constexpr uint32_t ADR_TU_5G_RO_FLAGS_2         = CSR_TU_RF_BASE + 0x0000061C;
constexpr uint32_t ADR_TU_DIGITAL_ADD_ON_3      = CSR_TU_RF_BASE + 0x0000080C;
constexpr uint32_t ADR_TU_DIGITAL_ADD_ON_4      = CSR_TU_RF_BASE + 0x00000810;
constexpr uint32_t ADR_TU_CAL_TIMER             = CSR_TU_RF_BASE + 0x00000534;
constexpr uint32_t ADR_TU_RF_D_CAL_TOP_0        = CSR_TU_RF_BASE + 0x00000834;
constexpr uint32_t ADR_TU_RF_D_CAL_TOP_1        = CSR_TU_RF_BASE + 0x00000838;
constexpr uint32_t ADR_TU_RF_D_CAL_TOP_2        = CSR_TU_RF_BASE + 0x0000083C;
constexpr uint32_t ADR_TU_RF_D_CAL_TOP_3        = CSR_TU_RF_BASE + 0x00000840;
constexpr uint32_t ADR_TU_RF_D_CAL_TOP_6        = CSR_TU_RF_BASE + 0x0000084C;
constexpr uint32_t ADR_TU_RF_D_CAL_TOP_9        = CSR_TU_RF_BASE + 0x00000858;
constexpr uint32_t ADR_TU_WIFI_PADPD_5G_BB_GAIN = CSR_TU_RF_BASE + 0x00000DA8;
constexpr uint32_t ADR_TU_5G_IQ_COMP_0          = CSR_TU_RF_BASE + 0x00000824;
constexpr uint32_t ADR_TU_5G_IQ_COMP_1          = CSR_TU_RF_BASE + 0x00000828;
constexpr uint32_t ADR_TU_5G_IQ_COMP_2          = CSR_TU_RF_BASE + 0x0000082C;
constexpr uint32_t ADR_TU_5G_IQ_COMP_3          = CSR_TU_RF_BASE + 0x00000830;

/* RF mode values (ADR_TU_MODE_REG.RG_MODE, bits [10:8]) */
constexpr uint8_t RF_MODE_SHUTDOWN = 0;
constexpr uint8_t RF_MODE_STANDBY  = 1;
constexpr uint8_t RF_MODE_TRX_EN   = 2;
constexpr uint32_t CBR_RG_MODE_MSK = 0x00000700U;
constexpr int CBR_RG_MODE_SFT      = 8;

/* ====================== PHY Registers ====================== */
constexpr uint32_t ADR_PHY_EN_0    = CSR_PHY_BASE + 0x00;
constexpr uint32_t ADR_PHY_EN_1    = CSR_PHY_BASE + 0x04;
constexpr uint32_t ADR_SVN_VERSION = CSR_PHY_BASE + 0x08;

/* PHY_EN_1 bit fields */
constexpr uint32_t RG_PHY_MD_EN_MSK = 0x00000001U;
constexpr int RG_PHY_MD_EN_SFT      = 0;

/* ====================== Calibration Bit Fields ====================== */
constexpr int RG_CAL_INDEX_SFT        = 12;
constexpr uint32_t RG_CAL_INDEX_MSK   = (0xFu << 12);
constexpr uint32_t RG_MODE_MANUAL_MSK = (1u << 2);

/* Calibration index values */
constexpr uint8_t CAL_IDX_NONE          = 0;
constexpr uint8_t CAL_IDX_WIFI2P4G_RXDC = 1;
constexpr uint8_t CAL_IDX_BW20_RXRC     = 3;
constexpr uint8_t CAL_IDX_WIFI2P4G_TXLO = 4;
constexpr uint8_t CAL_IDX_WIFI2P4G_TXIQ = 5;
constexpr uint8_t CAL_IDX_WIFI2P4G_RXIQ = 6;
constexpr uint8_t CAL_IDX_WIFI5G_RXDC   = 9;
constexpr uint8_t CAL_IDX_BW40_RXRC     = 10;
constexpr uint8_t CAL_IDX_WIFI5G_TXLO   = 11;
constexpr uint8_t CAL_IDX_WIFI5G_TXIQ   = 12;
constexpr uint8_t CAL_IDX_WIFI5G_RXIQ   = 13;

/* ADR_TU_RF_D_CAL_TOP_1 (0xCCB0A838) read-only status bits */
constexpr uint32_t RO_WF_DCCAL_DONE_MSK = (1u << 16);
constexpr uint32_t RO_RCCAL_DONE_MSK    = (1u << 18);
constexpr uint32_t RO_TXDC_DONE_MSK     = (1u << 19);
constexpr uint32_t RO_TXIQ_DONE_MSK     = (1u << 20);
constexpr uint32_t RO_RXIQ_DONE_MSK     = (1u << 21);
constexpr uint32_t RO_5G_RXIQ_DONE_MSK  = (1u << 24);
constexpr uint32_t RO_5G_DCCAL_DONE_MSK = (1u << 25);

/* 2.4G channel table bit fields */
constexpr int RG_SX_CHANNEL_SFT          = 11;
constexpr uint32_t RG_SX_CHANNEL_MSK     = (0x7Fu << 11);
constexpr uint32_t RG_SX_RFCH_MAP_EN_MSK = (1u << 3);

/* 5G channel table */
constexpr int RG_SX5GB_CHANNEL_SFT          = 8;
constexpr uint32_t RG_SX5GB_CHANNEL_MSK     = (0xFFu << 8);
constexpr uint32_t RG_SX5GB_RFCH_MAP_EN_MSK = (1u << 4);

/* ADR_TU_CAL_GAIN0 */
constexpr uint32_t RG_PGAG_RCCAL_MSK    = 0xFu;
constexpr int RG_PGAG_RCCAL_SFT         = 0;
constexpr uint32_t RG_TONE_SCALE_MSK    = (0x1FFu << 16);
constexpr int RG_TONE_SCALE_SFT         = 16;
constexpr uint32_t RG_TX_IQCAL_TIME_MSK = (3u << 20);
constexpr int RG_TX_IQCAL_TIME_SFT      = 20;

/* ADR_TU_CAL_TIMER */
constexpr int RG_RX_RCCAL_DELAY_SFT      = 8;
constexpr uint32_t RG_RX_RCCAL_DELAY_MSK = (7u << 8);

/* ADR_TU_RF_D_CAL_TOP_6 */
constexpr uint32_t RG_RX_RCCAL_TARG_MSK   = 0x3FFu;
constexpr int RG_RX_RCCAL_TARG_SFT        = 0;
constexpr uint32_t RG_RCCAL_POLAR_INV_MSK = (1u << 13);
constexpr int RG_RCCAL_POLAR_INV_SFT      = 13;

/* ADR_TU_5G_CAL_GAIN1 */
constexpr uint32_t RG_5G_RFG_RXIQCAL_MSK     = (3u << 4);
constexpr int RG_5G_RFG_RXIQCAL_SFT          = 4;
constexpr uint32_t RG_5G_PGAG_RXIQCAL_MSK    = (0xFu << 6);
constexpr int RG_5G_PGAG_RXIQCAL_SFT         = 6;
constexpr uint32_t RG_5G_TX_GAIN_RXIQCAL_MSK = (0x7Fu << 10);
constexpr int RG_5G_TX_GAIN_RXIQCAL_SFT      = 10;

/* ADR_TU_RF_D_CAL_TOP_0 */
constexpr int RG_ALPHA_SEL_SFT      = 20;
constexpr uint32_t RG_ALPHA_SEL_MSK = (3u << 20);

/* ====================== MIB Registers ====================== */
/* EDCCA channel survey registers from the kernel turismoC HAL. */
constexpr uint32_t ADR_WIFI_PHY_COMMON_EDCCA_0 = CSR_TU_PHY_BASE + 0x8CU;
constexpr uint32_t ADR_WIFI_PHY_COMMON_EDCCA_1 = CSR_TU_PHY_BASE + 0x90U;
constexpr uint32_t ADR_WIFI_PHY_COMMON_EDCCA_2 = CSR_TU_PHY_BASE + 0x94U;
constexpr uint32_t RG_EDCCA_AVG_T_MSK          = 0x00000007U;
constexpr uint32_t RG_EDCCA_STAT_EN_MSK        = 0x00000010U;
constexpr uint32_t RO_EDCCA_PRIMARY_PRD_MSK    = 0x0000FFFFU;
constexpr uint32_t RO_PRIMARY_EDCCA_MSK        = 0xFFFF0000U;
constexpr int RO_PRIMARY_EDCCA_SFT             = 16;
constexpr uint32_t RO_EDCCA_SECONDARY_PRD_MSK  = 0x0000FFFFU;
constexpr uint32_t RO_SECONDARY_EDCCA_MSK      = 0xFFFF0000U;
constexpr int RO_SECONDARY_EDCCA_SFT           = 16;
/* PHY 11g/n receive measurement register from the kernel HAL. */
constexpr uint32_t ADR_WIFI_11GN_RX_REG_246          = CSR_TU_PHY_BASE + 0x13D8U;
constexpr uint32_t RO_11GN_NOISE_PWR_MSK             = 0x00007F00U;
constexpr int RO_11GN_NOISE_PWR_SFT                  = 8;
constexpr uint32_t ADR_MIB_EN                        = MIB_REG_BASE + 0x00;
constexpr uint32_t ADR_MRX_FCS_ERR                   = MIB_REG_BASE + 0x1A0;
constexpr uint32_t ADR_MRX_FCS_SUCC                  = MIB_REG_BASE + 0x1A4;
constexpr uint32_t ADR_MRX_MISS                      = MIB_REG_BASE + 0x1A8;
constexpr uint32_t ADR_MRX_ALC_FAIL                  = MIB_REG_BASE + 0x1AC;
constexpr uint32_t ADR_RX_HOST_EVENT_COUNT           = HCI_REG_BASE + 0x120;
constexpr uint32_t ADR_WIFI_PHY_COMMON_RX_EN_CNT_REG = 0xCCB0E088;
constexpr uint32_t ADR_WIFI_PHY_COMMON_MAC_IF_CNT_RO = 0xCCB0E204;
constexpr uint32_t ADR_WIFI_PHY_COMMON_TOP_STATUS_RO = 0xCCB0E3C0;
constexpr uint32_t ADR_MRX_DATA_NTF                  = MIB_REG_BASE + 0x1C4;
constexpr uint32_t ADR_MRX_MNG_NTF                   = MIB_REG_BASE + 0x1C8;

/* ====================== SPI Registers ====================== */
constexpr uint32_t SPI_REG_BASE = 0xC0000A00;
constexpr uint32_t ADR_TX_SEG   = SPI_REG_BASE + 0x10;

/* ====================== Firmware Related ====================== */
constexpr uint32_t FW_START_ADDR          = 0x00;
constexpr uint8_t FIRMWARE_DOWNLOAD       = 0xF0;
constexpr uint8_t VENDOR_REG_RW           = 0x88;
constexpr uint8_t VENDOR_REG_RW_WDATA     = 0x99;
constexpr uint32_t FW_VERSION_REG         = ADR_TX_SEG;
constexpr uint32_t FIRWARE_NOT_MATCH_CODE = 0xF1F1F1F1;
constexpr uint32_t FW_CHECKSUM_INIT       = 0x12345678U;
constexpr uint32_t FW_STATUS_MASK         = 0x00FF0000U;

/* ====================== USB Endpoints ====================== */
constexpr uint8_t SSV_EP_CMD = 0x01;
constexpr uint8_t SSV_EP_RSP = 0x02;
constexpr uint8_t SSV_EP_TX  = 0x03;
constexpr uint8_t SSV_EP_RX  = 0x04;

/* ====================== USB Commands ====================== */
constexpr uint8_t SSV6200_CMD_WRITE_REG = 0x01;
constexpr uint8_t SSV6200_CMD_READ_REG  = 0x02;

/* ====================== HCI Host Command Types ====================== */
constexpr uint8_t HOST_CMD_CTYPE   = 5;
constexpr uint8_t HOST_EVENT_CTYPE = 6;

/* Host Command IDs */
constexpr uint8_t SSV6XXX_HOST_CMD_MRX_MODE  = 11;
constexpr uint8_t SSV6XXX_HOST_CMD_RFPHY_OPS = 13;
constexpr uint8_t SSV6XXX_HOST_CMD_SECURITY  = 14;
constexpr uint8_t SSV6XXX_HOST_CMD_RC_OPS    = 16;
constexpr uint8_t SSV6XXX_SECURITY_CMD_INIT  = 0;
constexpr uint8_t SSV6XXX_HOST_CMD_VIF_OPS   = 18;
constexpr uint8_t SSV6XXX_VIF_CMD_ADD        = 0;
constexpr uint8_t SSV6XXX_VIF_TYPE_STA       = 1;

/* HOST_CMD_RC_OPS sub-commands (matches kernel ssv6xxx_rc_ops). */
constexpr uint8_t SSV6XXX_RC_CMD_AUTO_RATE  = 3;
constexpr uint8_t SSV6XXX_RC_CMD_FIXED_RATE = 4;

/* MRX mode (ADR_MRX_FLT_TB13), matches kernel dev.h */
constexpr uint32_t MRX_MODE_PROMISCUOUS = 0x2;
constexpr uint32_t MRX_MODE_NORMAL      = 0x3;

/* HOST_CMD_MRX_MODE sub-commands */
constexpr uint8_t SSV6XXX_MRX_NORMAL      = 0;
constexpr uint8_t SSV6XXX_MRX_PROMISCUOUS = 1;

/* RFPHY Sub-commands */
constexpr uint8_t SSV6XXX_RFPHY_CMD_INIT_PLL_PHY_RF = 0;
constexpr uint8_t SSV6XXX_RFPHY_CMD_CHAN            = 1;
constexpr uint8_t SSV6XXX_RFPHY_CMD_RF_ENABLE       = 2;
constexpr uint8_t SSV6XXX_RFPHY_CMD_RF_DISABLE      = 3;
constexpr uint8_t SSV6XXX_RFPHY_CMD_PHY_ENABLE      = 4;
constexpr uint8_t SSV6XXX_RFPHY_CMD_PHY_DISABLE     = 5;
constexpr uint8_t SSV6XXX_RFPHY_CMD_TX_PWR          = 8;
constexpr uint8_t SSV6XXX_RFPHY_CMD_INIT_CALI       = 9;
constexpr uint8_t SSV6XXX_RFPHY_CMD_RESTORE_CALI    = 10;
constexpr uint8_t SSV6XXX_RFPHY_CMD_INIT_PLL        = 15;

/* RFPHY_CMD_TX_PWR payload, matching kernel struct ssv_rf_tx_pwr. */
struct SsvRfTxPower {
    uint32_t band;
    uint32_t pwr;
};
static_assert(sizeof(SsvRfTxPower) == 8, "SsvRfTxPower ABI must remain 8 bytes");

/* Channel types (nl80211_channel_type) */
constexpr uint8_t NL80211_CHAN_NO_HT     = 0;
constexpr uint8_t NL80211_CHAN_HT20      = 1;
constexpr uint8_t NL80211_CHAN_HT40MINUS = 2;
constexpr uint8_t NL80211_CHAN_HT40PLUS  = 3;

/* ====================== USB Vendor Info ====================== */
constexpr uint16_t USB_SSV_VENDOR_ID  = 0x8065;
constexpr uint16_t USB_SSV_PRODUCT_ID = 0x6000;

/* ====================== Default Config ====================== */
constexpr int TXPB_OFFSET         = 80;
constexpr int RXPB_OFFSET         = 80;
constexpr int SSV6XXX_TX_DESC_LEN = 80;
/* The SSV6006 RX descriptor is the 20-word (80-byte) descriptor used by the
 * kernel driver.  PHY information lives in WORD5-WORD7 inside this descriptor;
 * the separate 4-byte value is the HCI rx_pinfo_pad at the end of each MPDU,
 * not a prefix between the descriptor and the 802.11 frame. */
constexpr int SSV6XXX_RX_HW_DESC_LEN = 80;
constexpr int SSV6XXX_RX_PINFO_PAD   = 4;
constexpr int SSV6XXX_RX_DESC_LEN    = SSV6XXX_RX_HW_DESC_LEN;
constexpr int MAX_FRAME_SIZE         = 2432;
constexpr int MAX_HCI_RX_AGGR_SIZE   = 0x1B00;
constexpr int MAX_RX_PACKET_SIZE     = MAX_HCI_RX_AGGR_SIZE + MAX_FRAME_SIZE;
constexpr int HCI_RX_AGGR_SIZE       = 0x1B00;

/* Async RX URB buffer — must be large enough for aggregated transfers.
 * 16KB handles worst-case aggregation without OVERFLOW. */
constexpr int USB_RX_BUF_SIZE = 16384;

/* TX Queue sizes */
constexpr int SSV6200_ID_AC_BK_OUT_QUEUE = 8;
constexpr int SSV6200_ID_AC_BE_OUT_QUEUE = 15;
constexpr int SSV6200_ID_AC_VI_OUT_QUEUE = 16;
constexpr int SSV6200_ID_AC_VO_OUT_QUEUE = 16;
constexpr int SSV6200_ID_MANAGER_QUEUE   = 8;

/* Page / ID thresholds */
constexpr int SSV6200_TOTAL_ID            = 128;
constexpr int SSV6200_TOTAL_PAGE          = 256;
constexpr int SSV6200_ID_TX_THRESHOLD     = 63;
constexpr int SSV6200_ID_RX_THRESHOLD     = 63;
constexpr int SSV6200_PAGE_TX_THRESHOLD   = 126;
constexpr int SSV6200_PAGE_RX_THRESHOLD   = 126;
constexpr int SSV6200_TX_PKT_RSVD_SETTING = 3;
constexpr int SSV6200_TX_PKT_RSVD         = SSV6200_TX_PKT_RSVD_SETTING * 16;

/* ====================== HCI Host Command Structures ====================== */
struct SsvHostCmdHdr {
    uint32_t len : 16;
    uint32_t c_type : 3;
    uint32_t rsvd0 : 5;
    uint32_t h_cmd : 8;
    uint32_t sub_h_cmd;
    uint32_t cmd_seq_no;
    uint32_t blocking_seq_no;
} __attribute__((packed));

constexpr size_t HOST_CMD_HDR_LEN = 16;

struct SsvScanParam {
    uint8_t vif_idx;
    uint8_t wsid;
    uint8_t src_addr[6];
    uint8_t bssid[6];
    uint8_t ssid[64];
    uint32_t ssid_len;
    uint8_t no_cck;
    uint32_t ie_len;
    uint8_t ie[512];
    uint32_t reserved1;
    uint32_t reserved2;
} __attribute__((packed));

struct SsvRfChan {
    uint16_t chan;
    uint8_t chan_type;
    uint8_t off_chan;
    uint8_t scan;
    uint8_t passive_chan;
    SsvScanParam scan_param;
    uint32_t reserved1;
    uint32_t reserved2;
} __attribute__((packed));

static_assert(sizeof(SsvScanParam) == 607, "ssv_scan_param ABI mismatch");
static_assert(sizeof(SsvRfChan) == 621, "ssv_rf_chan ABI mismatch");

/* Firmware ABI from include/rf_table.h and struct ssv_rf_cali. */
struct SsvFwTempTable {
    uint8_t band_gain[7];
    uint8_t freq_xi, freq_xo;
    uint8_t ldo_rxafe, ldo_dcdcv, ldo_dldov;
    uint8_t pa_vcas1, pa_vcas2, pa_vcas3, pa_bias, pa_cap;
    uint8_t padpd_cali;
} __attribute__((packed));

struct SsvFwRateGain {
    uint8_t rate1, rate2, rate3, rate4;
} __attribute__((packed));
struct SsvFwTemp5gTable {
    uint8_t bbscale_band0, bbscale_band1, bbscale_band2, bbscale_band3;
    uint32_t bias1, bias2;
} __attribute__((packed));
struct SsvFwExtPaTable {
    uint8_t extpa_en, lna_trigger_2g, lna_trigger_5g;
    uint8_t rssi_lna_on_offset, rssi_lna_off_offset;
    uint8_t rssi_5g_lna_on_offset, rssi_5g_lna_off_offset, reserved;
} __attribute__((packed));
struct SsvFwRfTable {
    SsvFwTempTable rt_config, ht_config, lt_config;
    uint8_t rf_gain, rate_gain_b;
    SsvFwRateGain rate_config_g, rate_config_20n, rate_config_40n;
    int8_t low_boundary, high_boundary;
    uint8_t boot_flag, work_mode;
    SsvFwTemp5gTable rt_5g_config, ht_5g_config, lt_5g_config;
    uint16_t band_f0_threshold, band_f1_threshold, band_f2_threshold;
    uint8_t signature[4];
    uint32_t version, dcdc_flag;
    SsvFwExtPaTable extpa_tbl;
} __attribute__((packed));
struct SsvFwRfCali {
    uint32_t xtal;
    uint32_t options; // support_5g/thermal/.../bus_clk bit fields
    uint32_t thermal_thresholds;
    SsvFwRfTable rf_table;
} __attribute__((packed));

static_assert(sizeof(SsvFwRfTable) == 134, "st_rf_table ABI mismatch");
static_assert(sizeof(SsvFwRfCali) == 146, "ssv_rf_cali ABI mismatch");

struct SsvVifParam {
    uint8_t mac[6];
    uint8_t vif_idx;
    uint8_t type;
    uint8_t p2p;
    uint8_t assoc;
} __attribute__((packed));
static_assert(sizeof(SsvVifParam) == 10, "ssv_vif_param ABI mismatch");

/* ====================== Channel Table ====================== */
struct SsvChannel {
    uint16_t freq;
    uint8_t hw_idx;
};

constexpr SsvChannel ssv_2ghz_chantable[] = {
    {2412, 1},
    {2417, 2},
    {2422, 3},
    {2427, 4},
    {2432, 5},
    {2437, 6},
    {2442, 7},
    {2447, 8},
    {2452, 9},
    {2457, 10},
    {2462, 11},
    {2467, 12},
    {2472, 13},
    {2484, 14},
};

constexpr int PHY_INFO_TBL1_SIZE = 41;
constexpr int PHY_INFO_TBL2_SIZE = 16;
constexpr int PHY_INFO_TBL3_SIZE = 8;
