/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ZEPHYR_DRIVERS_SDHC_SDHCI_H_
#define __ZEPHYR_DRIVERS_SDHC_SDHCI_H_

#include <zephyr/sys/util.h>

/* SDMA System Address / Argument2 */
#define SDHCI_DMA_ADDRESS 0x00

/* SRS01 - Block Size / Block Count */
#define SDHCI_BLOCK_SIZE                0x04
#define SDHCI_BLOCK_SIZE_BCCT_POS       16
#define SDHCI_BLOCK_SIZE_BCCT_MASK      GENMASK(31, 16) /* Block Count for Current Transfer */
#define SDHCI_BLOCK_SIZE_SDMABB_POS     12
#define SDHCI_BLOCK_SIZE_SDMABB_MASK    GENMASK(14, 12) /* SDMA Buffer Boundary             */
#define SDHCI_BLOCK_SIZE_TBS_POS        0
#define SDHCI_BLOCK_SIZE_TBS_MASK       GENMASK(11, 0)  /* Transfer Block Size              */

/* Argument1 */
#define SDHCI_ARGUMENT 0x08

/* Command / Transfer Mode */
#define SDHCI_XFER_MODE 0x0C

#define SDHCI_XFER_MODE_CIDX_POS  24
#define SDHCI_XFER_MODE_CIDX_MASK GENMASK(29, 24) /* Command Index               */
#define SDHCI_XFER_MODE_CT_POS    22
#define SDHCI_XFER_MODE_CT_MASK   GENMASK(23, 22) /* Command Type                */
#define SDHCI_XFER_MODE_DPS       BIT(21)         /* Data Present Select         */
#define SDHCI_XFER_MODE_CICE      BIT(20)         /* Cmd Index Check Enable      */
#define SDHCI_XFER_MODE_CRCCE     BIT(19)         /* Cmd CRC Check Enable        */
#define SDHCI_XFER_MODE_SCF       BIT(18)         /* Sub Command Flag            */
#define SDHCI_XFER_MODE_RTS_POS   16
#define SDHCI_XFER_MODE_RTS_MASK  GENMASK(17, 16) /* Response Type Select        */
#define SDHCI_XFER_MODE_RID       BIT(8)          /* Response Interrupt Disable  */
#define SDHCI_XFER_MODE_RECE      BIT(7)          /* Response Error Check Enable */
#define SDHCI_XFER_MODE_RECT      BIT(6)          /* Response Error Check Type   */
#define SDHCI_XFER_MODE_MSBS      BIT(5)          /* Multi/Single Block Select   */
#define SDHCI_XFER_MODE_DTDS      BIT(4)          /* Data Transfer Dir Select    */
#define SDHCI_XFER_MODE_ACE_POS   2
#define SDHCI_XFER_MODE_ACE_MASK  GENMASK(3, 2)   /* Auto CMD Enable             */
#define SDHCI_XFER_MODE_BCE       BIT(1)          /* Block Count Enable          */
#define SDHCI_XFER_MODE_DMAE      BIT(0)          /* DMA Enable                  */

/* Response 0..3 (read-only) */
#define SDHCI_RESPONSE0 0x10
#define SDHCI_RESPONSE1 0x14
#define SDHCI_RESPONSE2 0x18
#define SDHCI_RESPONSE3 0x1C

/* Buffer Data Port */
#define SDHCI_BUFFER          0x20
#define SDHCI_BUFFER_BDP_POS  0
#define SDHCI_BUFFER_BDP_MASK GENMASK(31, 0)

/* Present State */
#define SDHCI_PRESENT_STATE 0x24

#define SDHCI_PRESENT_STATE_SCMDS       BIT(28)         /* Sub Command Status         */
#define SDHCI_PRESENT_STATE_CNIBE       BIT(27)         /* Cmd Not Issued by Err      */
#define SDHCI_PRESENT_STATE_LVSIRSLT    BIT(26)         /* Host Reg Volt Switch Rslt  */
#define SDHCI_PRESENT_STATE_CMDSL       BIT(24)         /* CMD Line Signal Level      */
#define SDHCI_PRESENT_STATE_DATSL1_POS  20
#define SDHCI_PRESENT_STATE_DATSL1_MASK GENMASK(23, 20) /* DAT[3:0] Line Signal Level */
#define SDHCI_PRESENT_STATE_WPSL        BIT(19)         /* Write Protect Switch Level */
#define SDHCI_PRESENT_STATE_CDSL        BIT(18)         /* Card Detect Pin Level      */
#define SDHCI_PRESENT_STATE_CSS         BIT(17)         /* Card State Stable          */
#define SDHCI_PRESENT_STATE_CI          BIT(16)         /* Card Inserted              */
#define SDHCI_PRESENT_STATE_BRE         BIT(11)         /* Buffer Read Enable         */
#define SDHCI_PRESENT_STATE_BWE         BIT(10)         /* Buffer Write Enable        */
#define SDHCI_PRESENT_STATE_RTA         BIT(9)          /* Read Transfer Active       */
#define SDHCI_PRESENT_STATE_WTA         BIT(8)          /* Write Transfer Active      */
#define SDHCI_PRESENT_STATE_DATSL2_POS  4
#define SDHCI_PRESENT_STATE_DATSL2_MASK GENMASK(7, 4)   /* DAT[7:4] Line Signal Level */
#define SDHCI_PRESENT_STATE_DLA         BIT(2)          /* Data Line Active           */
#define SDHCI_PRESENT_STATE_CIDAT       BIT(1)          /* Cmd Inhibit (DAT)          */
#define SDHCI_PRESENT_STATE_CICMD       BIT(0)          /* Cmd Inhibit (CMD)          */

/* Host Control 1 (General / Power / Block-Gap / Wake-Up) */
#define SDHCI_HOST_CTRL 0x28

#define SDHCI_HOST_CTRL_WORM        BIT(26)        /* Wake-Up on Removal        */
#define SDHCI_HOST_CTRL_WOIS        BIT(25)        /* Wake-Up on Insertion      */
#define SDHCI_HOST_CTRL_WOIQ        BIT(24)        /* Wake-Up on Card Int       */
#define SDHCI_HOST_CTRL_IBG         BIT(19)        /* Interrupt at Block Gap    */
#define SDHCI_HOST_CTRL_RWC         BIT(18)        /* Read Wait Control         */
#define SDHCI_HOST_CTRL_CREQ        BIT(17)        /* Continue Request          */
#define SDHCI_HOST_CTRL_SBGR        BIT(16)        /* Stop at Block Gap Request */
#define SDHCI_HOST_CTRL_BVS_POS     9
#define SDHCI_HOST_CTRL_BVS_MASK    GENMASK(11, 9) /* Bus Voltage Select        */
#define SDHCI_HOST_CTRL_BP          BIT(8)         /* Bus Power                 */
#define SDHCI_HOST_CTRL_CDSS        BIT(7)         /* Card Detect Signal Select */
#define SDHCI_HOST_CTRL_CDTL        BIT(6)         /* Card Detect Test Level    */
#define SDHCI_HOST_CTRL_EDTW        BIT(5)         /* Extended Data Xfer Width  */
#define SDHCI_HOST_CTRL_DMASEL_POS  3
#define SDHCI_HOST_CTRL_DMASEL_MASK GENMASK(4, 3)  /* DMA Select                */
#define SDHCI_HOST_CTRL_HSE         BIT(2)         /* High Speed Enable         */
#define SDHCI_HOST_CTRL_DTW         BIT(1)         /* Data Transfer Width       */
#define SDHCI_HOST_CTRL_LEDC        BIT(0)         /* LED Control               */

/* Clock Control / Timeout Control / Software Reset */
#define SDHCI_CLOCK_CTRL 0x2C

#define SDHCI_CLOCK_CTRL_SRDAT       BIT(26)         /* Software Reset for DAT     */
#define SDHCI_CLOCK_CTRL_SRCMD       BIT(25)         /* Software Reset for CMD     */
#define SDHCI_CLOCK_CTRL_SRFA        BIT(24)         /* Software Reset for All     */
#define SDHCI_CLOCK_CTRL_DTCV_POS    16
#define SDHCI_CLOCK_CTRL_DTCV_MASK   GENMASK(19, 16) /* Data Timeout Counter Value */
#define SDHCI_CLOCK_CTRL_SDCFSL_POS  8
#define SDHCI_CLOCK_CTRL_SDCFSL_MASK GENMASK(15, 8)  /* SDCLK Freq Select (low)    */
#define SDHCI_CLOCK_CTRL_SDCFSH_POS  6
#define SDHCI_CLOCK_CTRL_SDCFSH_MASK GENMASK(7, 6)   /* SDCLK Freq Select (high)   */
#define SDHCI_CLOCK_CTRL_SDCE        BIT(2)          /* SD Clock Enable            */
#define SDHCI_CLOCK_CTRL_ICS         BIT(1)          /* Internal Clock Stable      */
#define SDHCI_CLOCK_CTRL_ICE         BIT(0)          /* Internal Clock Enable      */

/* Error / Normal Interrupt Status */
#define SDHCI_INT_STATUS 0x30

#define SDHCI_INT_STATUS_ERSP   BIT(27) /* Error Response            */
#define SDHCI_INT_STATUS_EADMA  BIT(25) /* ADMA Error                */
#define SDHCI_INT_STATUS_EAC    BIT(24) /* Auto CMD Error            */
#define SDHCI_INT_STATUS_ECL    BIT(23) /* Current Limit Error       */
#define SDHCI_INT_STATUS_EDEB   BIT(22) /* Data End Bit Error        */
#define SDHCI_INT_STATUS_EDCRC  BIT(21) /* Data CRC Error            */
#define SDHCI_INT_STATUS_EDT    BIT(20) /* Data Timeout Error        */
#define SDHCI_INT_STATUS_ECI    BIT(19) /* Command Index Error       */
#define SDHCI_INT_STATUS_ECEB   BIT(18) /* Command End Bit Error     */
#define SDHCI_INT_STATUS_ECCRC  BIT(17) /* Command CRC Error         */
#define SDHCI_INT_STATUS_ECT    BIT(16) /* Command Timeout Error     */
#define SDHCI_INT_STATUS_EINT   BIT(15) /* Error Interrupt           */
#define SDHCI_INT_STATUS_CQINT  BIT(14) /* Command Queuing Interrupt */
#define SDHCI_INT_STATUS_FXE    BIT(13) /* Fixed Event               */
#define SDHCI_INT_STATUS_CINT   BIT(8)  /* Card Interrupt            */
#define SDHCI_INT_STATUS_CR     BIT(7)  /* Card Removal              */
#define SDHCI_INT_STATUS_CIN    BIT(6)  /* Card Insertion            */
#define SDHCI_INT_STATUS_BRR    BIT(5)  /* Buffer Read Ready         */
#define SDHCI_INT_STATUS_BWR    BIT(4)  /* Buffer Write Ready        */
#define SDHCI_INT_STATUS_DMAINT BIT(3)  /* DMA Interrupt             */
#define SDHCI_INT_STATUS_BGE    BIT(2)  /* Block Gap Event           */
#define SDHCI_INT_STATUS_TC     BIT(1)  /* Transfer Complete         */
#define SDHCI_INT_STATUS_CC     BIT(0)  /* Command Complete          */

/* Error / Normal Interrupt Status Enable */
#define SDHCI_INT_ENABLE 0x34

#define SDHCI_INT_ENABLE_ERSP_SE   BIT(27)
#define SDHCI_INT_ENABLE_EADMA_SE  BIT(25)
#define SDHCI_INT_ENABLE_EAC_SE    BIT(24)
#define SDHCI_INT_ENABLE_ECL_SE    BIT(23)
#define SDHCI_INT_ENABLE_EDEB_SE   BIT(22)
#define SDHCI_INT_ENABLE_EDCRC_SE  BIT(21)
#define SDHCI_INT_ENABLE_EDT_SE    BIT(20)
#define SDHCI_INT_ENABLE_ECI_SE    BIT(19)
#define SDHCI_INT_ENABLE_ECEB_SE   BIT(18)
#define SDHCI_INT_ENABLE_ECCRC_SE  BIT(17)
#define SDHCI_INT_ENABLE_ECT_SE    BIT(16)
#define SDHCI_INT_ENABLE_CQINT_SE  BIT(14)
#define SDHCI_INT_ENABLE_FXE_SE    BIT(13)
#define SDHCI_INT_ENABLE_CINT_SE   BIT(8)
#define SDHCI_INT_ENABLE_CR_SE     BIT(7)
#define SDHCI_INT_ENABLE_CIN_SE    BIT(6)
#define SDHCI_INT_ENABLE_BRR_SE    BIT(5)
#define SDHCI_INT_ENABLE_BWR_SE    BIT(4)
#define SDHCI_INT_ENABLE_DMAINT_SE BIT(3)
#define SDHCI_INT_ENABLE_BGE_SE    BIT(2)
#define SDHCI_INT_ENABLE_TC_SE     BIT(1)
#define SDHCI_INT_ENABLE_CC_SE     BIT(0)

/* Error / Normal Interrupt Signal Enable */
#define SDHCI_INT_SIGNAL_ENABLE 0x38

#define SDHCI_INT_SIGNAL_ENABLE_ERSP_IE   BIT(27)
#define SDHCI_INT_SIGNAL_ENABLE_EADMA_IE  BIT(25)
#define SDHCI_INT_SIGNAL_ENABLE_EAC_IE    BIT(24)
#define SDHCI_INT_SIGNAL_ENABLE_ECL_IE    BIT(23)
#define SDHCI_INT_SIGNAL_ENABLE_EDEB_IE   BIT(22)
#define SDHCI_INT_SIGNAL_ENABLE_EDCRC_IE  BIT(21)
#define SDHCI_INT_SIGNAL_ENABLE_EDT_IE    BIT(20)
#define SDHCI_INT_SIGNAL_ENABLE_ECI_IE    BIT(19)
#define SDHCI_INT_SIGNAL_ENABLE_ECEB_IE   BIT(18)
#define SDHCI_INT_SIGNAL_ENABLE_ECCRC_IE  BIT(17)
#define SDHCI_INT_SIGNAL_ENABLE_ECT_IE    BIT(16)
#define SDHCI_INT_SIGNAL_ENABLE_CQINT_IE  BIT(14)
#define SDHCI_INT_SIGNAL_ENABLE_FXE_IE    BIT(13)
#define SDHCI_INT_SIGNAL_ENABLE_CINT_IE   BIT(8)
#define SDHCI_INT_SIGNAL_ENABLE_CR_IE     BIT(7)
#define SDHCI_INT_SIGNAL_ENABLE_CIN_IE    BIT(6)
#define SDHCI_INT_SIGNAL_ENABLE_BRR_IE    BIT(5)
#define SDHCI_INT_SIGNAL_ENABLE_BWR_IE    BIT(4)
#define SDHCI_INT_SIGNAL_ENABLE_DMAINT_IE BIT(3)
#define SDHCI_INT_SIGNAL_ENABLE_BGE_IE    BIT(2)
#define SDHCI_INT_SIGNAL_ENABLE_TC_IE     BIT(1)
#define SDHCI_INT_SIGNAL_ENABLE_CC_IE     BIT(0)

/* SRS15 - Auto CMD Error Status / Host Control 2 */
#define SDHCI_AUTO_CMD_HOST_CTRL2 0x3C

#define SDHCI_HOST_CTRL2_PVE      BIT(31)         /* Preset Value Enable              */
#define SDHCI_HOST_CTRL2_A64B     BIT(29)         /* 64-bit Addressing                */
#define SDHCI_HOST_CTRL2_HV4E     BIT(28)         /* Host Version 4 Enable            */
#define SDHCI_HOST_CTRL2_CMD23E   BIT(27)         /* CMD23 Enable                     */
#define SDHCI_HOST_CTRL2_ADMA2LM  BIT(26)         /* ADMA2 Length Mode                */
#define SDHCI_HOST_CTRL2_LVSIEXEC BIT(24)         /* Voltage Switch Execute           */
#define SDHCI_HOST_CTRL2_SCS      BIT(23)         /* Sample Clock Select              */
#define SDHCI_HOST_CTRL2_EXTNG    BIT(22)         /* Execute Tuning                   */
#define SDHCI_HOST_CTRL2_DSS_POS  20
#define SDHCI_HOST_CTRL2_DSS_MASK GENMASK(21, 20) /* Driver Strength Select           */
#define SDHCI_HOST_CTRL2_V18SE    BIT(19)         /* 1.8V Signaling Enable            */
#define SDHCI_HOST_CTRL2_UMS_POS  16
#define SDHCI_HOST_CTRL2_UMS_MASK GENMASK(18, 16) /* UHS Mode Select                  */
#define SDHCI_AUTO_CMD_CNIACE     BIT(7)          /* Cmd Not Issued by Auto CMD12 Err */
#define SDHCI_AUTO_CMD_ACRE       BIT(5)          /* Auto CMD Response Error          */
#define SDHCI_AUTO_CMD_ACIE       BIT(4)          /* Auto CMD Index Error             */
#define SDHCI_AUTO_CMD_ACEBE      BIT(3)          /* Auto CMD End Bit Error           */
#define SDHCI_AUTO_CMD_ACCE       BIT(2)          /* Auto CMD CRC Error               */
#define SDHCI_AUTO_CMD_ACTE       BIT(1)          /* Auto CMD Timeout Error           */
#define SDHCI_AUTO_CMD_ACNE       BIT(0)          /* Auto CMD12 Not Executed          */

/* Capabilities */
#define SDHCI_CAPS1 0x40

#define SDHCI_CAPS1_SLT_POS      30
#define SDHCI_CAPS1_SLT_MASK     GENMASK(31, 30) /* Slot Type                 */
#define SDHCI_CAPS1_AIS          BIT(29)         /* Async Interrupt Support   */
#define SDHCI_CAPS1_A64SV3       BIT(28)         /* 64-bit Sys Addr (V3)      */
#define SDHCI_CAPS1_A64SV4       BIT(27)         /* 64-bit Sys Addr (V4)      */
#define SDHCI_CAPS1_VS18         BIT(26)         /* Voltage Support 1.8V      */
#define SDHCI_CAPS1_VS30         BIT(25)         /* Voltage Support 3.0V      */
#define SDHCI_CAPS1_VS33         BIT(24)         /* Voltage Support 3.3V      */
#define SDHCI_CAPS1_SRS          BIT(23)         /* Suspend/Resume Support    */
#define SDHCI_CAPS1_DMAS         BIT(22)         /* SDMA Support              */
#define SDHCI_CAPS1_HSS          BIT(21)         /* High Speed Support        */
#define SDHCI_CAPS1_ADMA1S       BIT(20)         /* ADMA1 Support             */
#define SDHCI_CAPS1_ADMA2S       BIT(19)         /* ADMA2 Support             */
#define SDHCI_CAPS1_EDS8         BIT(18)         /* 8-bit Embedded Support    */
#define SDHCI_CAPS1_MBL_POS      16
#define SDHCI_CAPS1_MBL_MASK     GENMASK(17, 16) /* Max Block Length          */
#define SDHCI_CAPS1_BCSDCLK_POS  8
#define SDHCI_CAPS1_BCSDCLK_MASK GENMASK(15, 8)  /* Base Clock Freq for SDCLK */
#define SDHCI_CAPS1_TCU          BIT(7)          /* Timeout Clock Unit        */
#define SDHCI_CAPS1_TCF_POS      0
#define SDHCI_CAPS1_TCF_MASK     GENMASK(5, 0)   /* Timeout Clock Frequency   */

/* Capabilities 2 */
#define SDHCI_CAPS2 0x44

#define SDHCI_CAPS2_LVSH         BIT(31)         /* 1.8V Signaling Support */
#define SDHCI_CAPS2_VDD2S        BIT(28)         /* VDD2 Support           */
#define SDHCI_CAPS2_ADMA3SUP     BIT(27)         /* ADMA3 Support          */
#define SDHCI_CAPS2_CLKMPR_POS   16
#define SDHCI_CAPS2_CLKMPR_MASK  GENMASK(23, 16) /* Clock Multiplier       */
#define SDHCI_CAPS2_RTNGM_POS    14
#define SDHCI_CAPS2_RTNGM_MASK   GENMASK(15, 14) /* Retuning Modes         */
#define SDHCI_CAPS2_UTSM50       BIT(13)         /* Use Tuning for SDR50   */
#define SDHCI_CAPS2_RTNGCNT_POS  8
#define SDHCI_CAPS2_RTNGCNT_MASK GENMASK(11, 8)  /* Retuning Count         */
#define SDHCI_CAPS2_DRVD         BIT(6)          /* Driver Type D Support  */
#define SDHCI_CAPS2_DRVC         BIT(5)          /* Driver Type C Support  */
#define SDHCI_CAPS2_DRVA         BIT(4)          /* Driver Type A Support  */
#define SDHCI_CAPS2_UHSII        BIT(3)          /* UHS-II Support         */
#define SDHCI_CAPS2_DDR50        BIT(2)          /* DDR50 Support          */
#define SDHCI_CAPS2_SDR104       BIT(1)          /* SDR104 Support         */
#define SDHCI_CAPS2_SDR50        BIT(0)          /* SDR50 Support          */

/* Max Current 1 Capabilities */
#define SDHCI_MAX_CURRENT1 0x48

#define SDHCI_MAX_CURRENT1_MC18_POS  16
#define SDHCI_MAX_CURRENT1_MC18_MASK GENMASK(23, 16) /* Max Current for 1.8V */
#define SDHCI_MAX_CURRENT1_MC30_POS  8
#define SDHCI_MAX_CURRENT1_MC30_MASK GENMASK(15, 8)  /* Max Current for 3.0V */
#define SDHCI_MAX_CURRENT1_MC33_POS  0
#define SDHCI_MAX_CURRENT1_MC33_MASK GENMASK(7, 0)   /* Max Current for 3.3V */

/* Max Current 2 Capabilities */
#define SDHCI_MAX_CURRENT2 0x4C

#define SDHCI_MAX_CURRENT2_MC18V2_POS  0
#define SDHCI_MAX_CURRENT2_MC18V2_MASK GENMASK(7, 0) /* Max Current for 1.8V VDD2 */

/* Force Event for Auto CMD / Error Status */
#define SDHCI_FORCE_EVENT 0x50

#define SDHCI_FORCE_EVENT_ERESP_FE  BIT(27)
#define SDHCI_FORCE_EVENT_ETUNE_FE  BIT(26)
#define SDHCI_FORCE_EVENT_EADMA_FE  BIT(25)
#define SDHCI_FORCE_EVENT_EAC_FE    BIT(24)
#define SDHCI_FORCE_EVENT_ECL_FE    BIT(23)
#define SDHCI_FORCE_EVENT_EDEB_FE   BIT(22)
#define SDHCI_FORCE_EVENT_EDCRC_FE  BIT(21)
#define SDHCI_FORCE_EVENT_EDT_FE    BIT(20)
#define SDHCI_FORCE_EVENT_ECI_FE    BIT(19)
#define SDHCI_FORCE_EVENT_ECEB_FE   BIT(18)
#define SDHCI_FORCE_EVENT_ECCRC_FE  BIT(17)
#define SDHCI_FORCE_EVENT_ECT_FE    BIT(16)
#define SDHCI_FORCE_EVENT_CNIACE_FE BIT(7)
#define SDHCI_FORCE_EVENT_ACIE_FE   BIT(4)
#define SDHCI_FORCE_EVENT_ACEBE_FE  BIT(3)
#define SDHCI_FORCE_EVENT_ACCE_FE   BIT(2)
#define SDHCI_FORCE_EVENT_ACTE_FE   BIT(1)
#define SDHCI_FORCE_EVENT_ACNE_FE   BIT(0)

/* ADMA Error Status              */
#define SDHCI_ADMA_ERROR 0x54

#define SDHCI_ADMA_ERROR_EADMAL      BIT(2)        /* ADMA Length Mismatch Error */
#define SDHCI_ADMA_ERROR_EADMAS_POS  0
#define SDHCI_ADMA_ERROR_EADMAS_MASK GENMASK(1, 0) /* ADMA Error State           */

/* ADMA System Address      */
#define SDHCI_ADMA_SYS_ADDR1 0x58
#define SDHCI_ADMA_SYS_ADDR2 0x5C

#define SDHCI_PRESET_VALUE0 0x60 /* Initial / Default Speed  */
#define SDHCI_PRESET_VALUE1 0x64 /* High Speed / SDR12       */
#define SDHCI_PRESET_VALUE2 0x68 /* SDR25 / SDR50            */
#define SDHCI_PRESET_VALUE3 0x6C /* SDR104 / DDR50           */

#define SDHCI_PRESET_DSSPV_LO_POS    14
#define SDHCI_PRESET_DSSPV_LO_MASK   GENMASK(15, 14) /* Driver Strength Select (low preset)  */
#define SDHCI_PRESET_SDCFSPV_LO_POS  0
#define SDHCI_PRESET_SDCFSPV_LO_MASK GENMASK(9, 0)   /* SDCLK Frequency Select (low preset)  */
#define SDHCI_PRESET_DSSPV_HI_POS    30
#define SDHCI_PRESET_DSSPV_HI_MASK   GENMASK(31, 30) /* Driver Strength Select (high preset) */
#define SDHCI_PRESET_SDCFSPV_HI_POS  16
#define SDHCI_PRESET_SDCFSPV_HI_MASK GENMASK(25, 16) /* SDCLK Frequency Select (high preset) */
#define SDHCI_PRESET_CGSPV_HI        BIT(26)         /* Clock Generator Select (high preset) */

/* ADMA3 Integrated Descriptor Address */
#define SDHCI_ADMA3_ID_ADDR1      0x78
#define SDHCI_ADMA3_ID_ADDR1_POS  0
#define SDHCI_ADMA3_ID_ADDR1_MASK GENMASK(31, 0)

#define SDHCI_ADMA3_ID_ADDR2      0x7C
#define SDHCI_ADMA3_ID_ADDR2_POS  0
#define SDHCI_ADMA3_ID_ADDR2_MASK GENMASK(31, 0)

enum sdhci_sw_reset {
	SDHCI_SW_RESET_DATA_LINE = 0,
	SDHCI_SW_RESET_CMD_LINE,
	SDHCI_SW_RESET_ALL
};

enum sdhci_cmd_type {
	SDHCI_CMD_NORMAL = 0,
	SDHCI_CMD_SUSPEND,
	SDHCI_CMD_RESUME,
	SDHCI_CMD_ABORT,
};

enum sdhci_response_type {
	SDHCI_RESP_NONE = 0,
	SDHCI_RESP_LEN_136,
	SDHCI_RESP_LEN_48,
	SDHCI_RESP_LEN_48B,
	SDHCI_INVAL_HOST_RESP_LEN,
};

struct sdhci_cmd_config {
	struct sdhc_command *sdhc_cmd;
	uint32_t cmd_idx;
	enum sdhci_cmd_type cmd_type;
	bool data_present;
	bool idx_check_en;
	bool crc_check_en;
};

#endif /* __ZEPHYR_DRIVERS_SDHC_SDHCI_H_ */
