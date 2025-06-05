/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

/*
 * GT9772AF voice coil motor driver
 *
 *
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/time.h>
#include <linux/uaccess.h>

#include "lens_info.h"

#define AF_DRVNAME "MOT_GT9772AF_DRV"
#define AF_I2C_SLAVE_ADDR 0x18

#define MOVE_CODE_STEP_MAX 40
#define WAIT_STABLE_TIME 8  // ms
#define AF_DEBUG
#ifdef AF_DEBUG
#define LOG_INF(format, args...)                                    \
	pr_info(AF_DRVNAME " [%s] " format, __func__, ##args)
#else
#define LOG_INF(format, args...)
#endif


static struct i2c_client *g_pstAF_I2Cclient;
static int *g_pAF_Opened;
static spinlock_t *g_pAF_SpinLock;

static unsigned long g_u4AF_INF = 0;
static unsigned long g_u4AF_MACRO = 1023;
static unsigned long g_u4CurrPosition = 0;
static unsigned long AF_STARTCODE_UP = 300;
static unsigned long AF_STARTCODE_DOWN = 150;
#define Min_Pos 0
#define Max_Pos 1023

static int s4AF_ReadReg(u8 a_uAddr, u8 *a_uData)
{
	g_pstAF_I2Cclient->addr = (AF_I2C_SLAVE_ADDR) >> 1;

	if (i2c_master_send(g_pstAF_I2Cclient, &a_uAddr, 1) < 0) {
		LOG_INF("ReadI2C send failed!!\n");
		return -1;
	}

	if (i2c_master_recv(g_pstAF_I2Cclient, a_uData, 1) < 0) {
		LOG_INF("ReadI2C recv failed!!\n");
		return -1;
	}


	return 0;
}

static int s4AF_WriteReg(u8 a_uLength, u8 a_uAddr, u16 a_u2Data)
{
	u8 puSendCmd[2] = {a_uAddr, (u8)(a_u2Data & 0xFF)};
	u8 puSendCmd2[3] = {a_uAddr, (u8)((a_u2Data >> 8) & 0xFF),
			    (u8)(a_u2Data & 0xFF)};

	g_pstAF_I2Cclient->addr = (AF_I2C_SLAVE_ADDR) >> 1;


	if (a_uLength == 0) {
		if (i2c_master_send(g_pstAF_I2Cclient, puSendCmd, 2) < 0) {
			LOG_INF("WriteI2C failed!!\n");
			return -1;
		}
	} else if (a_uLength == 1) {
		if (i2c_master_send(g_pstAF_I2Cclient, puSendCmd2, 3) < 0) {
			LOG_INF("WriteI2C 2 failed!!\n");
			return -1;
		}
	}
	/*LOG_INF("WriteI2C AF: %x %x\n",a_uAddr,a_u2Data);*/
	return 0;
}

static int setPosition(unsigned short UsPosition)
{
	unsigned short TarPos;
	unsigned char UcPosH;
	unsigned char UcPosL;
	unsigned int i4RetValue = 0;

	TarPos = UsPosition;


	UcPosH = (unsigned char)((TarPos >> 8) & 0x03);
	UcPosL = (unsigned char)(TarPos & 0x00FF);
	i4RetValue = s4AF_WriteReg(0, 0x03, UcPosH);//VCM_MSB
	if (i4RetValue != 0)
		return -1;
	i4RetValue = s4AF_WriteReg(0, 0x04, UcPosL); //VCM_LSB

	return i4RetValue;
}

static inline int getAFInfo(__user struct stAF_MotorInfo *pstMotorInfo)
{
	struct stAF_MotorInfo stMotorInfo;

	stMotorInfo.u4MacroPosition = g_u4AF_MACRO;
	stMotorInfo.u4InfPosition = g_u4AF_INF;
	stMotorInfo.u4CurrentPosition = g_u4CurrPosition;
	stMotorInfo.bIsSupportSR = 1;

	stMotorInfo.bIsMotorMoving = 1;

	if (*g_pAF_Opened >= 1)
		stMotorInfo.bIsMotorOpen = 1;
	else
		stMotorInfo.bIsMotorOpen = 0;

	if (copy_to_user(pstMotorInfo, &stMotorInfo,
			 sizeof(struct stAF_MotorInfo)))
		LOG_INF("copy to user failed when getting motor information\n");

	return 0;
}

/* initAF include driver initialization and standby mode */
static int initAF(void)
{
	LOG_INF(" beg-\n");
	if (*g_pAF_Opened == 1) {
		int i4RetValue = 0;
		char puSendCmdArray[3][2] = {
			{0x06, 0x84}, {0x07, 0x01}, {0x08, 0x55},
		};
		unsigned char cmd_number;
		i4RetValue = s4AF_WriteReg(0, 0xED, 0xAB); //advance mode
		LOG_INF("Advance mode ret: %x\n", i4RetValue);

		for (cmd_number = 0; cmd_number < 3; cmd_number++) {
			i4RetValue = i2c_master_send(g_pstAF_I2Cclient, puSendCmdArray[cmd_number], 2);
			mdelay(1);
			if (i4RetValue < 0)
			{
				LOG_INF("puSendCmdArray[%d] = %x, WriteI2C failed!!\n",cmd_number, puSendCmdArray[cmd_number]);
			}
			else
			{
				LOG_INF("puSendCmdArray[%d] = %x, WriteI2C success!!\n",cmd_number, puSendCmdArray[cmd_number]);
			}
		}
		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 2;
		spin_unlock(g_pAF_SpinLock);
	}
	LOG_INF(" end-\n");
	return 0;
}

/* moveAF only use to control moving the motor */
static inline int moveAF(unsigned long a_u4Position)
{

	int ret = 0;
	unsigned long m_cur_dac_code = 0;
/* debug
		unsigned char Temp;
		s4AF_ReadReg(0x03, &Temp);  //CODE MSB
		LOG_INF("REG03: %x\n", Temp);
		s4AF_ReadReg(0x04, &Temp);  //CODE LSB
		LOG_INF("REG04: %x\n", Temp);
*/
    if (!a_u4Position) {
		m_cur_dac_code = g_u4CurrPosition;
		if(m_cur_dac_code>(AF_STARTCODE_UP+MOVE_CODE_STEP_MAX)){
			m_cur_dac_code=AF_STARTCODE_UP+MOVE_CODE_STEP_MAX;
			setPosition((unsigned short)m_cur_dac_code);
			LOG_INF("release dac_target_code = %d\n", m_cur_dac_code);
			msleep(WAIT_STABLE_TIME);
			g_u4CurrPosition = m_cur_dac_code;
		}
        while ((m_cur_dac_code - a_u4Position) >= MOVE_CODE_STEP_MAX) {
            m_cur_dac_code = m_cur_dac_code - MOVE_CODE_STEP_MAX;
			setPosition((unsigned short)m_cur_dac_code);
			LOG_INF("release dac_target_code = %d\n", m_cur_dac_code);
			msleep(WAIT_STABLE_TIME);
			g_u4CurrPosition = m_cur_dac_code;
			if(m_cur_dac_code<AF_STARTCODE_DOWN){
				m_cur_dac_code=a_u4Position;
			}
        }
    }

	if (setPosition((unsigned short)a_u4Position) == 0) {
		g_u4CurrPosition = a_u4Position;
		ret = 0;
	} else {
		LOG_INF("set I2C failed when moving the motor\n");
		ret = -1;
	}

	return ret;
}

static inline int setAFInf(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_INF = a_u4Position;
	spin_unlock(g_pAF_SpinLock);

	return 0;
}

static inline int setAFMacro(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_MACRO = a_u4Position;
	spin_unlock(g_pAF_SpinLock);

	return 0;
}

/* ////////////////////////////////////////////////////////////// */
long MOT_MAUI_GT9772AF_Ioctl(struct file *a_pstFile, unsigned int a_u4Command,
		      unsigned long a_u4Param)
{
	long i4RetValue = 0;

	switch (a_u4Command) {
	case AFIOC_G_MOTORINFO:
		i4RetValue =
			getAFInfo((__user struct stAF_MotorInfo *)(a_u4Param));
		break;

	case AFIOC_T_MOVETO:
		i4RetValue = moveAF(a_u4Param);
		break;

	case AFIOC_T_SETINFPOS:
		i4RetValue = setAFInf(a_u4Param);
		break;

	case AFIOC_T_SETMACROPOS:
		i4RetValue = setAFMacro(a_u4Param);
		break;

	default:
		LOG_INF("No CMD\n");
		i4RetValue = -EPERM;
		break;
	}

	return i4RetValue;
}

/* Main jobs: */
/* 1.Deallocate anything that "open" allocated in private_data. */
/* 2.Shut down the device on last close. */
/* 3.Only called once on last time. */
/* Q1 : Try release multiple times. */
int MOT_MAUI_GT9772AF_Release(struct inode *a_pstInode, struct file *a_pstFile)
{
	int Ret = 0;
	unsigned char Temp;
	unsigned long m_cur_dac_code = 0;
	int minReleasePositon = 50;
	int releaseStep = 50;
	LOG_INF("Start\n");

	if (*g_pAF_Opened == 2) {

		LOG_INF("Wait\n");

		s4AF_ReadReg(0x03, &Temp);  //CODE MSB
		LOG_INF("REG03: %x\n", Temp);
		m_cur_dac_code = Temp;
		s4AF_ReadReg(0x04, &Temp);  //CODE LSB
		LOG_INF("REG04: %x\n", Temp);
		m_cur_dac_code = m_cur_dac_code*256 + Temp;
		g_u4CurrPosition = m_cur_dac_code;

		LOG_INF("current position = %d\n", g_u4CurrPosition);
		while(g_u4CurrPosition > minReleasePositon)
		{
			if(g_u4CurrPosition > (minReleasePositon + releaseStep*5))
			{
				setPosition(minReleasePositon + releaseStep*5);//minReleasePositon + releaseStep*5 = 300
				msleep(WAIT_STABLE_TIME);
				g_u4CurrPosition = minReleasePositon + releaseStep*5;
				LOG_INF("release dac_target_code = %d\n", g_u4CurrPosition);
			}
			else if(g_u4CurrPosition > (minReleasePositon + releaseStep*2) && g_u4CurrPosition <= (minReleasePositon + releaseStep*5))
			{
				setPosition(minReleasePositon + releaseStep*2);//minReleasePositon + releaseStep*2 = 150
				msleep(WAIT_STABLE_TIME);
				g_u4CurrPosition = minReleasePositon + releaseStep*2;
				LOG_INF("release dac_target_code = %d\n", g_u4CurrPosition);
			}
			else
			{
				setPosition(minReleasePositon);//minReleasePositon = 50
				msleep(WAIT_STABLE_TIME);
				g_u4CurrPosition = minReleasePositon;
				LOG_INF("release dac_target_code = %d\n", g_u4CurrPosition);
			}
		}
	}

	if (*g_pAF_Opened) {
		LOG_INF("Free\n");

		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 0;
		spin_unlock(g_pAF_SpinLock);
	}

	LOG_INF("End\n");

	return Ret;
}

int MOT_MAUI_GT9772AF_PowerDown(struct i2c_client *pstAF_I2Cclient,
			int *pAF_Opened)
{
	g_pstAF_I2Cclient = pstAF_I2Cclient;
	g_pAF_Opened = pAF_Opened;

	LOG_INF(" +\n");
	if (*g_pAF_Opened == 0) {
		LOG_INF("Set power donw +\n");
		LOG_INF("Set power donw -\n");
	}
	LOG_INF(" -\n");

	return 0;
}

int MOT_MAUI_GT9772AF_SetI2Cclient(struct i2c_client *pstAF_I2Cclient,
			    spinlock_t *pAF_SpinLock, int *pAF_Opened)
{
	mdelay(1);
	g_pstAF_I2Cclient = pstAF_I2Cclient;
	g_pAF_SpinLock = pAF_SpinLock;
	g_pAF_Opened = pAF_Opened;
	initAF();

	return 1;
}

int MOT_MAUI_GT9772AF_GetFileName(unsigned char *pFileName)
{
	#if SUPPORT_GETTING_LENS_FOLDER_NAME
	char FilePath[256];
	char *FileString;

	sprintf(FilePath, "%s", __FILE__);
	FileString = strrchr(FilePath, '/');
	*FileString = '\0';
	FileString = (strrchr(FilePath, '/') + 1);
	strncpy(pFileName, FileString, AF_MOTOR_NAME);
	LOG_INF("FileName : %s\n", pFileName);
	#else
	pFileName[0] = '\0';
	#endif
	return 1;
}
