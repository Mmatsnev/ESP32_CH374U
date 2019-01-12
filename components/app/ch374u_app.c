#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "log.h"
#include "ch374u_app.h"
#include "ch374u_hal.h"
#include "CH374INC.H"

// 附加的USB操作状态定义
#define ERR_USB_UNKNOWN 0xFA // 未知错误,不应该发生的情况,需检查硬件或者程序错误

uint8_t temp1 = 0, temp2 = 0;

// 获取设备描述符
uint8_t SetupGetDevDescr[] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00};
// 获取配置描述符
uint8_t SetupGetCfgDescr[] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x04, 0x00};
// 获取字符描述符
uint8_t SetupGetStrDescr[] = {0x80, 0x06, 0x00, 0x03, 0x09, 0x04, 0xFF, 0x00};
// 设置USB地址
uint8_t SetupSetUsbAddr[] = {0x00, 0x05, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
// 设置USB配置
uint8_t SetupSetUsbConfig[] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint8_t UsbDevEndpSize = DEFAULT_ENDP0_SIZE; /* USB设备的端点0的最大包尺寸 */

//USB设备相关信息表，CH374U最多支持3个设备
#define ROOT_DEV_DISCONNECT 0
#define ROOT_DEV_CONNECTED 1
#define ROOT_DEV_FAILED 2
#define ROOT_DEV_SUCCESS 3
struct _RootHubDev
{
	uint8_t DeviceStatus;  // 设备状态,0-无设备,1-有设备但尚未初始化,2-有设备但初始化枚举失败,3-有设备且初始化枚举成功
	uint8_t DeviceAddress; // 设备被分配的USB地址
	uint8_t DeviceSpeed;   // 0为低速,非0为全速
	uint8_t DeviceType;	// 设备类型
						   //	union {
						   //		struct MOUSE {
						   //			uint8_t	MouseInterruptEndp;		// 鼠标中断端点号
						   //			uint8_t	MouseIntEndpTog;		// 鼠标中断端点的同步标志
						   //			uint8_t	MouseIntEndpSize;		// 鼠标中断端点的长度
						   //		}
						   //		struct PRINT {
						   //		}
						   //	}
						   //.....    struct  _Endp_Attr   Endp_Attr[4];	//端点的属性,最多支持4个端点
	uint8_t GpVar;		   // 通用变量
} RootHubDev[3];

struct _DevOnHubPort
{
	uint8_t DeviceStatus;  // 设备状态,0-无设备,1-有设备但尚未初始化,2-有设备但初始化枚举失败,3-有设备且初始化枚举成功
	uint8_t DeviceAddress; // 设备被分配的USB地址
	uint8_t DeviceSpeed;   // 0为低速,非0为全速
	uint8_t DeviceType;	// 设备类型
						   //.....    struct  _Endp_Attr   Endp_Attr[4];	//端点的属性,最多支持4个端点
	uint8_t GpVar;		   // 通用变量
} DevOnHubPort[3][4];	  // 假定:不超过三个外部HUB,每个外部HUB不超过4个端口(多了不管)

uint8_t NewDevCount;
uint8_t CtrlBuf[8];
uint8_t TempBuf[64];

// CH374传输事务，输入目的端点地址/PID令牌/同步标志，返回同CH375，NAK不重试，超时/出错重试
uint8_t HostTransact374(uint8_t endp_addr, uint8_t pid, bool tog);

// CH374传输事务，输入目的端点地址/PID令牌/同步标志/以mS为单位的NAK重试总时间(0xFFFF无限重试)，返回同CH375，NAK重试，超时出错重试
uint8_t WaitHostTransact374(uint8_t endp_addr, uint8_t pid, bool tog, uint16_t timeout);

uint8_t HostCtrlTransfer374(uint8_t *ReqBuf, uint8_t *DatBuf, uint8_t *RetLen); // 执行控制传输,ReqBuf指向8字节请求码,DatBuf为收发缓冲区
// 如果需要接收和发送数据，那么DatBuf需指向有效缓冲区用于存放后续数据，实际成功收发的总长度保存在ReqLen指向的字节变量中

void HostDetectInterrupt(uint8_t inter_flag_reg); // 处理USB设备插拔事件中断

void SetHostUsbAddr(uint8_t addr); // 设置USB主机当前操作的USB设备地址

void HostEnableRootHub(void); // 启用内置的Root-HUB

void Init374Host(void); // 初始化USB主机

uint8_t GetDeviceDescr(uint8_t *buf); // 获取设备描述符

uint8_t GetConfigDescr(uint8_t *buf); // 获取配置描述符

uint8_t SetUsbAddress(uint8_t addr); // 设置USB设备地址

uint8_t SetUsbConfig(uint8_t cfg); // 设置USB设备配置

uint8_t GetHubDescriptor(void); // 获取HUB描述符

uint8_t GetPortStatus(uint8_t port); // 查询HUB端口状态

uint8_t SetPortFeature(uint8_t port, uint8_t select);

uint8_t ClearPortFeature(uint8_t port, uint8_t select);

void DisableRootHubPort(uint8_t index); // 关闭指定的ROOT-HUB端口,实际上硬件已经自动关闭,此处只是清除一些结构状态

void ResetRootHubPort(uint8_t index); // 检测到设备后,复位相应端口的总线,为枚举设备准备,设置为默认为全速

bool EnableRootHubPort(uint8_t index); // 使能ROOT-HUB端口,相应的BIT_HUB?_EN置1开启端口,返回FALSE设置失败(可能是设备断开了)

void SetUsbSpeed(bool FullSpeed); // 设置当前USB速度

void SelectHubPort(uint8_t HubIndex, uint8_t PortIndex); // PortIndex=0选择操作指定的ROOT-HUB端口,否则选择操作指定的ROOT-HUB端口的外部HUB的指定端口

void AnalyzeRootHub(void); // 分析ROOT-HUB状态,处理ROOT-HUB端口的设备插拔事件
//处理HUB端口的插拔事件，如果设备拔出，函数中调用DisableHubPort()函数，将端口关闭，插入事件，置相应端口的状态位

uint8_t AnalyzeHidIntEndp(void); // 从描述符中分析出HID中断端点的地址

uint8_t InitDevice(uint8_t index); // 初始化/枚举指定ROOT-HUB端口的USB设备

uint8_t HubPortEnum(uint8_t index); // 枚举指定ROOT-HUB端口上的外部HUB集线器的各个端口,检查各端口有无连接或移除事件

uint8_t Level2DevEnum(uint8_t HubIndex, uint8_t PortIndex); // 初始化枚举外部HUB后的二级USB设备

uint8_t SearchRootHubPort(uint8_t type); // 搜索指定类型的设备所在的端口号,输出端口号为0xFF则未搜索到

uint16_t SearchAllHubPort(uint8_t type); // 在ROOT-HUB以及外部HUB各端口上搜索指定类型的设备所在的端口号,输出端口号为0xFFFF则未搜索到
// 输出高8位为ROOT-HUB端口号,低8位为外部HUB的端口号,低8位为0则设备直接在ROOT-HUB端口上

void mDelaymS(uint16_t t)
{
	vTaskDelay(t / portTICK_RATE_MS);
}

void mDelayuS(uint16_t t)
{
	ets_delay_us(t);
}

// 查询CH374中断(INT#低电平)
bool Query374Interrupt(uint8_t *inter_flag_reg)
{
	if (inter_flag_reg == NULL)
	{
		if (Read374Byte(REG_INTER_FLAG) & BIT_IF_INTER_FLAG) /* 如果未连接CH374的中断引脚则查询中断标志寄存器 */
		{
			return true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		*inter_flag_reg = Read374Byte(REG_INTER_FLAG);
		if (*inter_flag_reg & BIT_IF_INTER_FLAG) /* 如果未连接CH374的中断引脚则查询中断标志寄存器 */
		{
			return true;
		}
		else
		{
			return false;
		}
	}
	return false;
}

// 等待CH374中断(INT#低电平)，超时则返回ERR_USB_UNKNOWN
uint8_t Wait374Interrupt(void)
{
	uint16_t i = 0;
	for (i = 0; i < 10000; i++)
	{ // 计数防止超时
		if (Query374Interrupt(NULL))
		{
			return 0;
		}
	}
	return ERR_USB_UNKNOWN; // 不应该发生的情况
}

// CH374传输事务，输入目的端点地址/PID令牌/同步标志，返回同CH375，NAK不重试，超时/出错重试
uint8_t HostTransact374(uint8_t endp_addr, uint8_t pid, bool tog)
{ // 本子程序着重于易理解,而在实际应用中,为了提供运行速度,应该对本子程序代码进行优化
	uint8_t retry;
	uint8_t s, r;
	for (retry = 0; retry < 3; retry++)
	{
		Write374Byte(REG_USB_H_PID, M_MK_HOST_PID_ENDP(pid, endp_addr));												 // 指定令牌PID和目的端点号
																														 //		Write374Byte( REG_USB_H_CTRL, BIT_HOST_START | ( tog ? ( BIT_HOST_TRAN_TOG | BIT_HOST_RECV_TOG ) : 0x00 ) );  // 设置同步标志并启动传输
		Write374Byte(REG_USB_H_CTRL, (tog ? (BIT_HOST_START | BIT_HOST_TRAN_TOG | BIT_HOST_RECV_TOG) : BIT_HOST_START)); // 设置同步标志并启动传输
																														 //		Write374Byte( REG_INTER_FLAG, BIT_IF_USB_PAUSE );  // 取消暂停
		s = Wait374Interrupt();
		if (s == ERR_USB_UNKNOWN)
			return (s);					 // 中断超时,可能是硬件异常
		s = Read374Byte(REG_INTER_FLAG); // 获取中断状态
		if (s & BIT_IF_DEV_DETECT)
		{													 // USB设备插拔事件
															 //			mDelayuS( 200 );  // 等待传输完成
			AnalyzeRootHub();								 // 分析ROOT-HUB状态
			Write374Byte(REG_INTER_FLAG, BIT_IF_DEV_DETECT); // 清中断标志
			s = Read374Byte(REG_INTER_FLAG);				 // 获取中断状态
			if ((s & BIT_IF_DEV_ATTACH) == 0x00)
				return (USB_INT_DISCONNECT); // USB设备断开事件
		}
		if (s & BIT_IF_TRANSFER)
		{																	  // 传输完成
			Write374Byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_TRANSFER); // 清中断标志
			s = Read374Byte(REG_USB_STATUS);								  // USB状态
			r = s & BIT_STAT_DEV_RESP;										  // USB设备应答状态
			switch (pid)
			{
			case DEF_USB_PID_SETUP:
			case DEF_USB_PID_OUT:
				if (r == DEF_USB_PID_ACK)
					return (USB_INT_SUCCESS);
				else if (r == DEF_USB_PID_STALL || r == DEF_USB_PID_NAK)
					return (r | 0x20);
				else if (!M_IS_HOST_TIMEOUT(s))
					return (r | 0x20); // 不是超时/出错，意外应答
				break;
			case DEF_USB_PID_IN:
				if (M_IS_HOST_IN_DATA(s))
				{ // DEF_USB_PID_DATA0 or DEF_USB_PID_DATA1
					if (s & BIT_STAT_TOG_MATCH)
						return (USB_INT_SUCCESS); // 不同步则需丢弃后重试
				}
				else if (r == DEF_USB_PID_STALL || r == DEF_USB_PID_NAK)
					return (r | 0x20);
				else if (!M_IS_HOST_TIMEOUT(s))
					return (r | 0x20); // 不是超时/出错，意外应答
				break;
			default:
				return (ERR_USB_UNKNOWN); // 不可能的情况
				break;
			}
		}
		else
		{																		// 其它中断,不应该发生的情况
			mDelayuS(200);														// 等待传输完成
			Write374Byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_INTER_FLAG); /* 清中断标志 */
			if (retry)
				return (ERR_USB_UNKNOWN); /* 不是第一次检测到则返回错误 */
		}
	}
	return (0x20); // 应答超时
}

// CH374传输事务，输入目的端点地址/PID令牌/同步标志/以mS为单位的NAK重试总时间(0xFFFF无限重试)，返回同CH375，NAK重试，超时出错重试
uint8_t WaitHostTransact374(uint8_t endp_addr, uint8_t pid, bool tog, uint16_t timeout)
{
	uint8_t i, s;
	while (1)
	{
		for (i = 0; i < 40; i++)
		{
			s = HostTransact374(endp_addr, pid, tog);
			if (s != (DEF_USB_PID_NAK | 0x20) || timeout == 0)
				return (s);
			mDelayuS(20);
		}
		if (timeout < 0xFFFF)
			timeout--;
	}
}

uint8_t HostCtrlTransfer374(uint8_t *ReqBuf, uint8_t *DatBuf, uint8_t *RetLen) // 执行控制传输,ReqBuf指向8字节请求码,DatBuf为收发缓冲区
// 如果需要接收和发送数据，那么DatBuf需指向有效缓冲区用于存放后续数据，实际成功收发的总长度保存在ReqLen指向的字节变量中
{
	uint8_t s, len, count, total = 0;
	bool tog;
	Write374Block(RAM_HOST_TRAN, 8, ReqBuf);
	Write374Byte(REG_USB_LENGTH, 8);
	mDelayuS(200);
	s = WaitHostTransact374(0, DEF_USB_PID_SETUP, false, 200); // SETUP阶段，200mS超时
	if (s == USB_INT_SUCCESS)
	{				// SETUP成功
		tog = true; // 默认DATA1,默认无数据故状态阶段为IN
		total = *(ReqBuf + 6);
		if (total && DatBuf)
		{ // 需要收发数据
			len = total;
			if (*ReqBuf & 0x80)
			{ // 收
				while (len)
				{
					//mDelayuS( 400 );
					s = WaitHostTransact374(0, DEF_USB_PID_IN, tog, 200); // IN数据
					if (s != USB_INT_SUCCESS)
						break;
					count = Read374Byte(REG_USB_LENGTH);
					Read374Block(RAM_HOST_RECV, count, DatBuf);
					DatBuf += count;
					if (count <= len)
						len -= count;
					else
						len = 0;
					if (count == 0 || (count & (UsbDevEndpSize - 1)))
						break; // 短包
					tog = tog ? false : true;
				}
				tog = false; // 状态阶段为OUT
			}
			else
			{ // 发
				while (len)
				{
					//mDelayuS( 400 );
					count = len >= UsbDevEndpSize ? UsbDevEndpSize : len;
					Write374Block(RAM_HOST_TRAN, count, DatBuf);
					Write374Byte(REG_USB_LENGTH, count);
					s = WaitHostTransact374(0, DEF_USB_PID_OUT, tog, 200); // OUT数据
					if (s != USB_INT_SUCCESS)
						break;
					DatBuf += count;
					len -= count;
					tog = tog ? false : true;
				}
				tog = true; // 状态阶段为IN
			}
			total -= len; // 减去剩余长度得实际传输长度
		}
		if (s == USB_INT_SUCCESS)
		{ // 数据阶段成功
			Write374Byte(REG_USB_LENGTH, 0);
			mDelayuS(200);
			s = WaitHostTransact374(0, (tog ? DEF_USB_PID_IN : DEF_USB_PID_OUT), true, 200); // STATUS阶段
			if (tog && s == USB_INT_SUCCESS)
			{ // 检查IN状态返回数据长度
				if (Read374Byte(REG_USB_LENGTH))
					s = USB_INT_BUF_OVER; // 状态阶段错误
			}
		}
	}
	if (RetLen)
		*RetLen = total; // 实际成功收发的总长度
	return (s);
}

void HostDetectInterrupt(uint8_t inter_flag_reg) // 处理USB设备插拔事件中断
{

	if (inter_flag_reg & BIT_IF_DEV_DETECT)
	{													 // USB设备插拔事件
		AnalyzeRootHub();								 // 分析ROOT-HUB状态
		Write374Byte(REG_INTER_FLAG, BIT_IF_DEV_DETECT); // 清中断标志
	}
	else
	{																											// 意外的中断
		Write374Byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_TRANSFER | BIT_IF_USB_SUSPEND | BIT_IF_WAKE_UP); // 清中断标志
	}
}

void SetHostUsbAddr(uint8_t addr) // 设置USB主机当前操作的USB设备地址
{
	Write374Byte(REG_USB_ADDR, addr);
}

void HostEnableRootHub(void) // 启用内置的Root-HUB
{

	//	Write374Byte( REG_USB_SETUP, M_SET_USB_BUS_FREE( Read374Byte( REG_USB_SETUP ) ) );  // USB总线空闲
	Write374Byte(REG_USB_SETUP, BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF); // USB主机方式,允许SOF

	Write374Byte(REG_HUB_SETUP, 0x00); // 清BIT_HUB_DISABLE,允许内置的ROOT-HUB
	Write374Byte(REG_HUB_CTRL, 0x00);  // 清除ROOT-HUB信息
}

void Init374Host(void) // 初始化USB主机
{

	Write374Byte(REG_USB_SETUP, 0x00);
	SetHostUsbAddr(0x00);
	Write374Byte(REG_USB_H_CTRL, 0x00);
	Write374Byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_INTER_FLAG); // 清所有中断标志
																		//	Write374Byte( REG_INTER_EN, BIT_IE_TRANSFER );  // 允许传输完成中断,因为本程序使用查询方式检测USB设备插拔,所以无需USB设备检测中断
	Write374Byte(REG_INTER_EN, BIT_IE_TRANSFER | BIT_IE_DEV_DETECT);	// 允许传输完成中断和USB设备检测中断
																		//	Write374Byte( REG_SYS_CTRL, BIT_CTRL_OE_POLAR  );  // 对于CH374T或者UEN引脚悬空的CH374S必须置BIT_CTRL_OE_POLAR为1
	Write374Byte(REG_USB_SETUP, BIT_SETP_HOST_MODE);					// USB主机方式
	HostEnableRootHub();												// 启用内置的Root-HUB
}

uint8_t GetDeviceDescr(uint8_t *buf) // 获取设备描述符
{
	uint8_t s, len;
	UsbDevEndpSize = DEFAULT_ENDP0_SIZE;
	SetupGetDevDescr[6] = 0x08;
	s = HostCtrlTransfer374(SetupGetDevDescr, buf, &len); // 执行控制传输
	if (s == USB_INT_SUCCESS)
	{
		if (len == ((PUSB_SETUP_REQ)SetupGetDevDescr)->wLengthL)
		{
			SetupGetDevDescr[6] = buf[0];
			s = HostCtrlTransfer374(SetupGetDevDescr, buf, &len); // 执行控制传输
			if (s == USB_INT_SUCCESS)
			{
				if (len != ((PUSB_SETUP_REQ)SetupGetDevDescr)->wLengthL)
				{
					s = USB_INT_BUF_OVER; // 描述符长度错误
				}
				else
				{
					UsbDevEndpSize = ((PUSB_DEV_DESCR)buf)->bMaxPacketSize0; // 端点0最大包长度
				}
			}
		}
		else
		{
			s = USB_INT_BUF_OVER; // 描述符长度错误
		}
	}
	return (s);
}

uint8_t GetConfigDescr(uint8_t *buf) // 获取配置描述符
{
	uint8_t s, len;
	s = HostCtrlTransfer374(SetupGetCfgDescr, buf, &len); // 执行控制传输
	if (s == USB_INT_SUCCESS)
	{
		if (len < ((PUSB_SETUP_REQ)SetupGetCfgDescr)->wLengthL)
			s = USB_INT_BUF_OVER; // 返回长度错误
		else
		{
			len = ((PUSB_CFG_DESCR)buf)->wTotalLengthL;
			memcpy(CtrlBuf, SetupGetCfgDescr, sizeof(SetupGetCfgDescr));
			((PUSB_SETUP_REQ)CtrlBuf)->wLengthL = len;   // 完整配置描述符的总长度
			s = HostCtrlTransfer374(CtrlBuf, buf, &len); // 执行控制传输
			if (s == USB_INT_SUCCESS)
			{
				if (len < ((PUSB_CFG_DESCR)buf)->wTotalLengthL)
					s = USB_INT_BUF_OVER; // 描述符长度错误
			}
		}
	}
	return (s);
}

uint8_t SetUsbAddress(uint8_t addr) // 设置USB设备地址
{
	uint8_t s;
	memcpy(CtrlBuf, SetupSetUsbAddr, sizeof(SetupSetUsbAddr));
	((PUSB_SETUP_REQ)CtrlBuf)->wValueL = addr;	// USB设备地址
	s = HostCtrlTransfer374(CtrlBuf, NULL, NULL); // 执行控制传输
	if (s == USB_INT_SUCCESS)
	{
		SetHostUsbAddr(addr); // 设置USB主机当前操作的USB设备地址
	}
	mDelaymS(10); // 等待USB设备完成操作
	return (s);
}

uint8_t SetUsbConfig(uint8_t cfg) // 设置USB设备配置
{
	memcpy(CtrlBuf, SetupSetUsbConfig, sizeof(SetupSetUsbConfig));
	((PUSB_SETUP_REQ)CtrlBuf)->wValueL = cfg;		   // USB设备配置
	return (HostCtrlTransfer374(CtrlBuf, NULL, NULL)); // 执行控制传输
}

uint8_t GetHubDescriptor(void) // 获取HUB描述符
{
	uint8_t s, len;
	CtrlBuf[0] = GET_HUB_DESCRIPTOR;
	CtrlBuf[1] = GET_DESCRIPTOR;
	CtrlBuf[2] = 0x00;
	CtrlBuf[3] = 0x29;
	CtrlBuf[4] = 0x00;
	CtrlBuf[5] = 0x00;
	CtrlBuf[6] = 0x01;
	CtrlBuf[7] = 0x00;
	s = HostCtrlTransfer374(CtrlBuf, TempBuf, &len); // 执行控制传输
	if (s == USB_INT_SUCCESS)
	{
		CtrlBuf[6] = TempBuf[0];
		CtrlBuf[0] = GET_HUB_DESCRIPTOR;
		CtrlBuf[1] = GET_DESCRIPTOR;
		CtrlBuf[2] = 0x00;
		CtrlBuf[3] = 0x29;
		CtrlBuf[4] = 0x00;
		CtrlBuf[5] = 0x00;
		CtrlBuf[7] = 0x00;
		s = HostCtrlTransfer374(CtrlBuf, TempBuf, &len); // 执行控制传输
	}
	return s;
}

uint8_t GetPortStatus(uint8_t port) // 查询HUB端口状态
{
	uint8_t s, len;
	CtrlBuf[0] = GET_PORT_STATUS;
	CtrlBuf[1] = GET_STATUS;
	CtrlBuf[2] = 0x00;
	CtrlBuf[3] = 0x00;
	CtrlBuf[4] = port;
	CtrlBuf[5] = 0x00;
	CtrlBuf[6] = 4;
	CtrlBuf[7] = 0x00;
	s = HostCtrlTransfer374(CtrlBuf, TempBuf, &len); // 执行控制传输
	return s;
}

uint8_t SetPortFeature(uint8_t port, uint8_t select)
{
	uint8_t s, len;
	CtrlBuf[0] = SET_PORT_FEATURE;
	CtrlBuf[1] = SET_FEATURE;
	CtrlBuf[2] = select;
	CtrlBuf[3] = 0x00;
	CtrlBuf[4] = port;
	CtrlBuf[5] = 0x00;
	CtrlBuf[6] = 0x00;
	CtrlBuf[7] = 0x00;
	s = HostCtrlTransfer374(CtrlBuf, TempBuf, &len); // 执行控制传输
	return s;
}

uint8_t ClearPortFeature(uint8_t port, uint8_t select)
{
	uint8_t s, len;
	CtrlBuf[0] = CLEAR_PORT_FEATURE;
	CtrlBuf[1] = CLEAR_FEATURE;
	CtrlBuf[2] = select;
	CtrlBuf[3] = 0x00;
	CtrlBuf[4] = port;
	CtrlBuf[5] = 0x00;
	CtrlBuf[6] = 0x00;
	CtrlBuf[7] = 0x00;
	s = HostCtrlTransfer374(CtrlBuf, TempBuf, &len); // 执行控制传输
	return s;
}

void DisableRootHubPort(uint8_t index) // 关闭指定的ROOT-HUB端口,实际上硬件已经自动关闭,此处只是清除一些结构状态
{
	RootHubDev[index].DeviceStatus = ROOT_DEV_DISCONNECT;
	RootHubDev[index].DeviceAddress = 0x00;

	if (index == 1)
	{
		Write374Byte(REG_HUB_CTRL, Read374Byte(REG_HUB_CTRL) & 0xF0); // 清除有关HUB1的控制数据,实际上不需要清除
	}
	else if (index == 2)
	{
		Write374Byte(REG_HUB_CTRL, Read374Byte(REG_HUB_CTRL) & 0x0F); // 清除有关HUB2的控制数据,实际上不需要清除
	}
	else
	{
		Write374Byte(REG_HUB_SETUP, Read374Byte(REG_HUB_SETUP) & 0xF0); // 清除有关HUB0的控制数据,实际上不需要清除
	}
	//	printf( "HUB %01x close\n",(uint16_t)index );
}

void ResetRootHubPort(uint8_t index) // 检测到设备后,复位相应端口的总线,为枚举设备准备,设置为默认为全速
{
	UsbDevEndpSize = DEFAULT_ENDP0_SIZE; /* USB设备的端点0的最大包尺寸 */
	SetHostUsbAddr(0x00);
	Write374Byte(REG_USB_H_CTRL, 0x00);
	if (index == 1)
	{
		Write374Byte(REG_HUB_CTRL, (Read374Byte(REG_HUB_CTRL) & ~BIT_HUB1_POLAR) | BIT_HUB1_RESET); // 默认为全速,开始复位
		mDelaymS(15);																				// 复位时间10mS到20mS
		Write374Byte(REG_HUB_CTRL, Read374Byte(REG_HUB_CTRL) & ~BIT_HUB1_RESET);					// 结束复位
	}
	else if (index == 2)
	{
		Write374Byte(REG_HUB_CTRL, (Read374Byte(REG_HUB_CTRL) & ~BIT_HUB2_POLAR) | BIT_HUB2_RESET); // 默认为全速,开始复位
		mDelaymS(15);																				// 复位时间10mS到20mS
		Write374Byte(REG_HUB_CTRL, Read374Byte(REG_HUB_CTRL) & ~BIT_HUB2_RESET);					// 结束复位
	}
	else
	{
		Write374Byte(REG_HUB_SETUP, (Read374Byte(REG_HUB_SETUP) & ~BIT_HUB0_POLAR) | BIT_HUB0_RESET); // 默认为全速,开始复位
		mDelaymS(15);																				  // 复位时间10mS到20mS
		Write374Byte(REG_HUB_SETUP, Read374Byte(REG_HUB_SETUP) & ~BIT_HUB0_RESET);					  // 结束复位
	}
	mDelayuS(250);
	Write374Byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_DEV_DETECT | BIT_IF_USB_SUSPEND); // 清中断标志
}

bool EnableRootHubPort(uint8_t index) // 使能ROOT-HUB端口,相应的BIT_HUB?_EN置1开启端口,返回FALSE设置失败(可能是设备断开了)
{
	uint8_t hub_ctrl_reg = Read374Byte(REG_HUB_CTRL);
	uint8_t hub_setup_reg = Read374Byte(REG_HUB_SETUP);

	if (RootHubDev[index].DeviceStatus < ROOT_DEV_CONNECTED)
	{
		RootHubDev[index].DeviceStatus = ROOT_DEV_CONNECTED;
	}

	if (index == 1)
	{
		if (hub_ctrl_reg & BIT_HUB1_ATTACH)
		{ // 有设备
			if (!(hub_ctrl_reg & BIT_HUB1_EN))
			{ // 尚未使能
				if (!(hub_setup_reg & BIT_HUB1_DX_IN))
				{
					Write374Byte(REG_HUB_CTRL, hub_ctrl_reg ^ BIT_HUB1_POLAR); // 如果速度不匹配则设置极性
					hub_ctrl_reg = Read374Byte(REG_HUB_CTRL);
				}
				RootHubDev[1].DeviceSpeed = !(hub_ctrl_reg & BIT_HUB1_POLAR);
			}
			Write374Byte(REG_HUB_CTRL, hub_ctrl_reg | BIT_HUB1_EN); //使能HUB端口
			return (true);
		}
	}
	else if (index == 2)
	{
		if (hub_ctrl_reg & BIT_HUB2_ATTACH)
		{ // 有设备
			if (!(hub_ctrl_reg & BIT_HUB2_EN))
			{ // 尚未使能
				if (!(hub_setup_reg & BIT_HUB2_DX_IN))
				{
					Write374Byte(REG_HUB_CTRL, hub_ctrl_reg ^ BIT_HUB2_POLAR); // 如果速度不匹配则设置极性
					hub_ctrl_reg = Read374Byte(REG_HUB_CTRL);
				}
				RootHubDev[2].DeviceSpeed = !(hub_ctrl_reg & BIT_HUB2_POLAR);
			}
			Write374Byte(REG_HUB_CTRL, hub_ctrl_reg | BIT_HUB2_EN); //使能HUB端口
			return (true);
		}
	}
	else
	{
		if (hub_setup_reg & BIT_HUB0_ATTACH)
		{ // 有设备
			if (!(hub_setup_reg & BIT_HUB0_EN))
			{ // 尚未使能
				if (!(Read374Byte(REG_INTER_FLAG) & BIT_HUB0_DX_IN))
				{
					Write374Byte(REG_HUB_SETUP, hub_setup_reg ^ BIT_HUB0_POLAR); // 如果速度不匹配则设置极性
					hub_setup_reg = Read374Byte(REG_HUB_SETUP);
				}
				RootHubDev[0].DeviceSpeed = !(hub_setup_reg & BIT_HUB0_POLAR);
			}
			Write374Byte(REG_HUB_SETUP, hub_setup_reg | BIT_HUB0_EN); //使能HUB端口
			return (true);
		}
	}
	return (false);
}

void SetUsbSpeed(bool FullSpeed) // 设置当前USB速度
{
	if (FullSpeed)
	{																															// 全速
		Write374Byte(REG_USB_SETUP, (Read374Byte(REG_USB_SETUP) & BIT_SETP_RAM_MODE) | BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF); // 全速
		Write374Byte(REG_HUB_SETUP, Read374Byte(REG_HUB_SETUP) & ~BIT_HUB_PRE_PID);												// 禁止PRE PID
	}
	else
		Write374Byte(REG_USB_SETUP, (Read374Byte(REG_USB_SETUP) & BIT_SETP_RAM_MODE) | BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF | BIT_SETP_LOW_SPEED); // 低速
}

void SelectHubPort(uint8_t HubIndex, uint8_t PortIndex) // PortIndex=0选择操作指定的ROOT-HUB端口,否则选择操作指定的ROOT-HUB端口的外部HUB的指定端口
{
	if (PortIndex)
	{																				   // 选择操作指定的ROOT-HUB端口的外部HUB的指定端口
		SetHostUsbAddr(DevOnHubPort[HubIndex][PortIndex - 1].DeviceAddress);		   // 设置USB主机当前操作的USB设备地址
		if (DevOnHubPort[HubIndex][PortIndex - 1].DeviceSpeed == 0)					   // 通过外部HUB与低速USB设备通讯需要前置ID
			Write374Byte(REG_HUB_SETUP, Read374Byte(REG_HUB_SETUP) | BIT_HUB_PRE_PID); // 启用PRE PID
		SetUsbSpeed(DevOnHubPort[HubIndex][PortIndex - 1].DeviceSpeed);				   // 设置当前USB速度
	}
	else
	{														// 选择操作指定的ROOT-HUB端口
		SetHostUsbAddr(RootHubDev[HubIndex].DeviceAddress); // 设置USB主机当前操作的USB设备地址
		SetUsbSpeed(RootHubDev[HubIndex].DeviceSpeed);		// 设置当前USB速度
	}
}

//处理HUB端口的插拔事件，如果设备拔出，函数中调用DisableHubPort()函数，将端口关闭，插入事件，置相应端口的状态位
void AnalyzeRootHub(void) // 分析ROOT-HUB状态,处理ROOT-HUB端口的设备插拔事件
{
	uint8_t hub_setup_reg = 0, hub_ctrl_reg = 0;
	hub_setup_reg = Read374Byte(REG_HUB_SETUP);
	hub_ctrl_reg = Read374Byte(REG_HUB_CTRL);

	if (hub_setup_reg & BIT_HUB0_ATTACH)
	{
		if (RootHubDev[0].DeviceStatus == ROOT_DEV_DISCONNECT)
		{
			DisableRootHubPort(0);							 // 关闭端口
			RootHubDev[0].DeviceStatus = ROOT_DEV_CONNECTED; //置连接标志
			printf("HUB 0 device in\n");
			NewDevCount++;
		}
	}
	else
	{
		if (RootHubDev[0].DeviceStatus >= ROOT_DEV_CONNECTED)
		{
			DisableRootHubPort(0); // 关闭端口
			printf("HUB 0 device out\n");
		}
	}

	if (hub_ctrl_reg & BIT_HUB1_ATTACH)
	{
		if (RootHubDev[1].DeviceStatus == ROOT_DEV_DISCONNECT)
		{
			DisableRootHubPort(1);							 // 关闭端口
			RootHubDev[1].DeviceStatus = ROOT_DEV_CONNECTED; //置连接标志
			printf("HUB 1 device in\n");
			NewDevCount++;
		}
	}
	else
	{
		if (RootHubDev[1].DeviceStatus >= ROOT_DEV_CONNECTED)
		{
			DisableRootHubPort(1); // 关闭端口
			printf("HUB 1 device out\n");
		}
	}

	if (hub_ctrl_reg & BIT_HUB2_ATTACH)
	{
		if (RootHubDev[2].DeviceStatus == ROOT_DEV_DISCONNECT)
		{
			DisableRootHubPort(2);							 // 关闭端口
			RootHubDev[2].DeviceStatus = ROOT_DEV_CONNECTED; //置连接标志
			printf("HUB 2 device in\n");
			NewDevCount++;
		}
	}
	else
	{
		if (RootHubDev[2].DeviceStatus >= ROOT_DEV_CONNECTED)
		{
			DisableRootHubPort(2); // 关闭端口
			printf("HUB 2 device out\n");
		}
	}
}

uint8_t AnalyzeHidIntEndp(void) // 从描述符中分析出HID中断端点的地址
{
	uint8_t i, s, l;
	s = 0;
	for (i = 0; i < ((PUSB_CFG_DESCR)TempBuf)->wTotalLengthL; i += l)
	{																				 // 搜索中断端点描述符,跳过配置描述符和接口描述符
		if (((PUSB_ENDP_DESCR)(TempBuf + i))->bDescriptorType == USB_ENDP_DESCR_TYPE // 是端点描述符
			&& ((PUSB_ENDP_DESCR)(TempBuf + i))->bmAttributes == USB_ENDP_TYPE_INTER // 是中断端点
			&& (((PUSB_ENDP_DESCR)(TempBuf + i))->bEndpointAddress & 0x80))
		{																   // 是IN端点
			s = ((PUSB_ENDP_DESCR)(TempBuf + i))->bEndpointAddress & 0x7F; // 中断端点的地址
			break;														   // 可以根据需要保存wMaxPacketSize和bInterval
		}
		l = ((PUSB_ENDP_DESCR)(TempBuf + i))->bLength; // 当前描述符长度,跳过
		if (l > 16)
			break;
	}
	return (s);
}

void PrintfDeviceDescr(PUSB_DEV_DESCR dev_descr)
{
	printf("==========DeviceDescr Start==========\r\n");
	printf("bLength:\t\t%02X\r\n", dev_descr->bLength);
	printf("bDescriptorType:\t%02X\r\n", dev_descr->bDescriptorType);
	printf("bcdUSBL:\t\t%02X\r\n", dev_descr->bcdUSBL);
	printf("bcdUSBH:\t\t%02X\r\n", dev_descr->bcdUSBH);
	printf("bDeviceClass:\t\t%02X\r\n", dev_descr->bDeviceClass);
	printf("bDeviceSubClass:\t%02X\r\n", dev_descr->bDeviceSubClass);
	printf("bDeviceProtocol:\t%02X\r\n", dev_descr->bDeviceProtocol);
	printf("bMaxPacketSize0:\t%02X\r\n", dev_descr->bMaxPacketSize0);
	printf("idVendorL:\t\t%02X\r\n", dev_descr->idVendorL);
	printf("idVendorH:\t\t%02X\r\n", dev_descr->idVendorH);
	printf("idProductL:\t\t%02X\r\n", dev_descr->idProductL);
	printf("idProductH:\t\t%02X\r\n", dev_descr->idProductH);
	printf("bcdDeviceL:\t\t%02X\r\n", dev_descr->bcdDeviceL);
	printf("bcdDeviceH:\t\t%02X\r\n", dev_descr->bcdDeviceH);
	printf("iManufacturer:\t\t%02X\r\n", dev_descr->iManufacturer);
	printf("iProduct:\t\t%02X\r\n", dev_descr->iProduct);
	printf("iSerialNumber:\t\t%02X\r\n", dev_descr->iSerialNumber);
	printf("bNumConfigurations:\t%02X\r\n", dev_descr->bNumConfigurations);
	printf("==========DeviceDescr End==========\r\n");
}

void PrintfConfigDescr(PUSB_CFG_DESCR config_descr)
{

	printf("bLength:\t\t%02X\r\n", config_descr->bLength);
	printf("bDescriptorType:\t%02X\r\n", config_descr->bDescriptorType);
	printf("wTotalLengthL:\t\t%02X\r\n", config_descr->wTotalLengthL);
	printf("wTotalLengthH:\t\t%02X\r\n", config_descr->wTotalLengthH);
	printf("bNumInterfaces:\t\t%02X\r\n", config_descr->bNumInterfaces);
	printf("bConfigurationValue:\t%02X\r\n", config_descr->bConfigurationValue);
	printf("iConfiguration:\t\t%02X\r\n", config_descr->iConfiguration);
	printf("bmAttributes:\t\t%02X\r\n", config_descr->bmAttributes);
	printf("MaxPower:\t\t%02X\r\n", config_descr->MaxPower);
}

void PrintfItfDescr(PUSB_ITF_DESCR itf_descr)
{

	printf("\tbLength:\t\t%02X\r\n", itf_descr->bLength);
	printf("\tbDescriptorType:\t%02X\r\n", itf_descr->bDescriptorType);
	printf("\tbInterfaceNumber:\t%02X\r\n", itf_descr->bInterfaceNumber);
	printf("\tbAlternateSetting:\t%02X\r\n", itf_descr->bAlternateSetting);
	printf("\tbNumEndpoints:\t\t%02X\r\n", itf_descr->bNumEndpoints);
	printf("\tbInterfaceClass:\t%02X\r\n", itf_descr->bInterfaceClass);
	printf("\tbInterfaceSubClass:\t%02X\r\n", itf_descr->bInterfaceSubClass);
	printf("\tbInterfaceProtocol:\t%02X\r\n", itf_descr->bInterfaceProtocol);
	printf("\tiInterface:\t\t%02X\r\n", itf_descr->iInterface);
}

void PrintfHIDDescr(PUSB_HID_DESCR hid_descr)
{
	printf("\t==========HIDDescr Start==========\r\n");
	printf("\tbLength:\t\t%02X\r\n", hid_descr->bLength);
	printf("\tbDescriptorType:\t%02X\r\n", hid_descr->bDescriptorType);
	printf("\tbcdHIDL:\t\t%02X\r\n", hid_descr->bcdHIDL);
	printf("\tbcdHIDH:\t\t%02X\r\n", hid_descr->bcdHIDH);
	printf("\tbCountryCode:\t\t%02X\r\n", hid_descr->bCountryCode);
	printf("\tbNumDescriptors:\t%02X\r\n", hid_descr->bNumDescriptors);
	printf("\tbDescriptorType2:\t%02X\r\n", hid_descr->bDescriptorType2);
	printf("\tbDescriptorLengthL:\t%02X\r\n", hid_descr->bDescriptorLengthL);
	printf("\tbDescriptorLengthH:\t%02X\r\n", hid_descr->bDescriptorLengthH);
	printf("\t==========HIDDescr End==========\r\n");
}
void PrintfEndpDescr(PUSB_ENDP_DESCR endp_descr)
{

	printf("\t\tbLength:\t\t%02X\r\n", endp_descr->bLength);
	printf("\t\tbDescriptorType:\t%02X\r\n", endp_descr->bDescriptorType);
	printf("\t\tbEndpointAddress:\t%02X\r\n", endp_descr->bEndpointAddress);
	printf("\t\tbmAttributes:\t\t%02X\r\n", endp_descr->bmAttributes);
	printf("\t\twMaxPacketSize:\t\t%02X\r\n", endp_descr->wMaxPacketSize);
	printf("\t\twMaxPacketSize1:\t%02X\r\n", endp_descr->wMaxPacketSize1);
	printf("\t\tbInterval:\t\t%02X\r\n", endp_descr->bInterval);
}

uint8_t InitHIDDevice(uint8_t cfg,uint8_t index,uint8_t InterfaceProtocol)
{
	uint8_t s;
	s = SetUsbConfig(cfg); // 设置USB设备配置
	if (s == USB_INT_SUCCESS)
	{
		//						Set_Idle( );
		//						需保存端点信息以便主程序进行USB传输
		s = AnalyzeHidIntEndp();	 // 从描述符中分析出HID中断端点的地址
		RootHubDev[index].GpVar = s; // 保存中断端点的地址,位7用于同步标志位,清0
		RootHubDev[index].DeviceStatus = ROOT_DEV_SUCCESS;
		SetUsbSpeed(true); // 默认为全速
		if (InterfaceProtocol == 1)
		{
			//							进一步初始化,例如设备键盘指示灯LED等
			printf("USB-Keyboard Ready\n");
			return (DEV_KEYBOARD); /* 键盘初始化成功 */
		}
		else if (InterfaceProtocol == 2)
		{
			//							为了以后查询鼠标状态,应该分析描述符,取得中断端口的地址,长度等信息
			printf("USB-Mouse Ready\n");
			return (DEV_MOUSE); /* 鼠标初始化成功 */
		}
	}

	return (DEV_ERROR);
}

uint8_t InitADBDevice(uint8_t cfg,uint8_t index)
{
	uint8_t s;
	s = SetUsbConfig(cfg); // 设置USB设备配置
	if (s == USB_INT_SUCCESS)
	{
		RootHubDev[index].DeviceStatus = ROOT_DEV_SUCCESS;
		SetUsbSpeed( true );  // 默认为全速
		printf("ADB Ready\n");
		return (DEV_ADB); /* U盘初始化成功 */
	}

	return (DEV_ERROR);
}

uint8_t GetStringDescr(uint8_t str_index) // 获取设备描述符
{
	uint8_t s, len,str_buf[256];

	SetupGetStrDescr[2] = str_index;
	s = HostCtrlTransfer374(SetupGetStrDescr, str_buf, &len); // 执行控制传输
	if (s == USB_INT_SUCCESS)
	{
		printf("GetStringDescr:");
		printf_byte_str(str_buf,len);
	}
	return (s);
}

uint8_t InitDevice(uint8_t index) // 初始化/枚举指定ROOT-HUB端口的USB设备
{
	uint8_t i, s, cfg, dv_cls, if_cls, if_scls,if_protocol;
	uint8_t *config_index = TempBuf, endp_num = 0, itf_num = 0, itf_count = 0, endp_count = 0;

	printf("Start reset HUB%01d port\n", (uint16_t)index);
	ResetRootHubPort(index); // 检测到设备后,复位相应端口的USB总线
	for (i = 0, s = 0; i < 100; i++)
	{ // 等待USB设备复位后重新连接
		if (EnableRootHubPort(index))
		{ // 使能ROOT-HUB端口
			i = 0;
			s++; // 计时等待USB设备连接后稳定
			if (s > 100)
				break; // 已经稳定连接
		}
		mDelaymS(1);
	}

	if (i)
	{ // 复位后设备没有连接
		DisableRootHubPort(index);
		printf("Disable HUB%01d port because of disconnect\r\n", (uint16_t)index);
		return (DEV_ERROR);
	}
	if (RootHubDev[index].DeviceSpeed)
	{
		printf("full speed\r\n");
	}
	else
	{
		printf("low speed\r\n");
	}

	SetUsbSpeed(RootHubDev[index].DeviceSpeed); // 设置当前USB速度
	printf("GetDeviceDescr @HUB%1d:\r\n", (uint16_t)index);

	s = GetDeviceDescr(TempBuf); // 获取设备描述符
	if (s == USB_INT_SUCCESS)
	{
		GetStringDescr(((PUSB_DEV_DESCR)TempBuf)->iManufacturer);
		GetStringDescr(((PUSB_DEV_DESCR)TempBuf)->iProduct);
		GetStringDescr(((PUSB_DEV_DESCR)TempBuf)->iSerialNumber);

		printf_byte(TempBuf, ((PUSB_SETUP_REQ)SetupGetDevDescr)->wLengthL); // 显示出描述符
		PrintfDeviceDescr((PUSB_DEV_DESCR)TempBuf);

		dv_cls = ((PUSB_DEV_DESCR)TempBuf)->bDeviceClass;					   // 设备类代码
		s = SetUsbAddress(index + ((PUSB_SETUP_REQ)SetupSetUsbAddr)->wValueL); // 设置USB设备地址,加上index可以保证三个HUB端口分配不同的地址
		if (s == USB_INT_SUCCESS)
		{
			RootHubDev[index].DeviceAddress = index + ((PUSB_SETUP_REQ)SetupSetUsbAddr)->wValueL; // 保存USB地址
			printf("SetDeviceAddress:%02X\r\n",RootHubDev[index].DeviceAddress);
			printf("GetConfigDescr: ");
			s = GetConfigDescr(TempBuf); // 获取配置描述符
			if (s == USB_INT_SUCCESS)
			{
				cfg = ((PUSB_CFG_DESCR)TempBuf)->bConfigurationValue;

				printf_byte(TempBuf, ((PUSB_CFG_DESCR)config_index)->wTotalLengthL);

				printf("==========ConfigDescr Start==========\r\n");

				PrintfConfigDescr((PUSB_CFG_DESCR)config_index);
				itf_num = ((PUSB_CFG_DESCR)config_index)->bNumInterfaces;
				config_index += 9;
				GetStringDescr(((PUSB_CFG_DESCR)config_index)->iConfiguration);

				for (itf_count = 0; itf_count < itf_num; itf_count++)
				{
					printf("\t==========ItfDescr %d Start==========\r\n", itf_count);

					PrintfItfDescr((PUSB_ITF_DESCR)config_index);
					GetStringDescr(((PUSB_ITF_DESCR)config_index)->iInterface);

					/* 分析配置描述符，获取端点数据/各端点地址/各端点大小等，更新变量endp_addr和endp_size等 */
					if_cls = ((PUSB_ITF_DESCR)config_index)->bInterfaceClass; // 接口类代码
					if_scls = ((PUSB_ITF_DESCR)config_index)->bInterfaceSubClass;
					if_protocol = ((PUSB_ITF_DESCR)config_index)->bInterfaceProtocol;

					printf("%02X %02X %02X \r\n", dv_cls, if_cls, if_scls);

					endp_num = ((PUSB_ITF_DESCR)config_index)->bNumEndpoints;
					config_index += 9;

					if (if_cls == 0x03)
					{
						PrintfHIDDescr((PUSB_HID_DESCR)config_index);
						config_index += 9;
					}

					for (endp_count = 0; endp_count < endp_num; endp_count++)
					{
						printf("\t\t==========EndpDescr %d Start==========\r\n", endp_count);
						PrintfEndpDescr((PUSB_ENDP_DESCR)(config_index));
						config_index += 7;
						printf("\t\t==========EndpDescr End==========\r\n");
					}
					printf("\t==========ItfDescr End==========\r\n");

					if (dv_cls == 0x00 && if_cls == 0x03 && if_scls <= 0x01)
					{						   // 是HID类设备,键盘/鼠标等
						return InitHIDDevice(cfg,index,if_protocol);
					}else if (dv_cls == 0x00 && if_cls == 0xFF && if_scls == 0x42)//ADB设备
					{
						return InitADBDevice(cfg,index);
					}
					
				}
				printf("==========ConfigDescr End==========\r\n");

				// if (dv_cls == 0x00 && if_cls == 0x08)
				// {						   // 是USB存储类设备,基本上确认是U盘
				// 	s = SetUsbConfig(cfg); // 设置USB设备配置
				// 	if (s == USB_INT_SUCCESS)
				// 	{
				// 		RootHubDev[index].DeviceStatus = ROOT_DEV_SUCCESS;
				// 		//						SetUsbSpeed( true );  // 默认为全速
				// 		printf("USB-Disk Ready\n");
				// 		return (DEV_DISK); /* U盘初始化成功 */
				// 	}
				// }
				// else if (dv_cls == 0x00 && if_cls == 0x07 && ((PUSB_CFG_DESCR_LONG)TempBuf)->itf_descr.bInterfaceSubClass == 0x01)
				// {						   // 是打印机类设备
				// 	s = SetUsbConfig(cfg); // 设置USB设备配置
				// 	if (s == USB_INT_SUCCESS)
				// 	{
				// 		//						需保存端点信息以便主程序进行USB传输
				// 		RootHubDev[index].DeviceStatus = ROOT_DEV_SUCCESS;
				// 		SetUsbSpeed(true); // 默认为全速
				// 		printf("USB-Print Ready\n");
				// 		return (DEV_PRINT); /* 打印机初始化成功 */
				// 	}
				// }


				// else if (dv_cls == 0x09)
				// { // 是HUB类设备,集线器等
				// 	printf("GetHubDescriptor: ");
				// 	s = GetHubDescriptor();
				// 	if (s == USB_INT_SUCCESS)
				// 	{
				// 		for (i = 0; i < TempBuf[0]; i++)
				// 			printf("0x%02X ", (uint16_t)(TempBuf[i]));
				// 		printf("\n");
				// 		RootHubDev[index].GpVar = ((PHUBDescr)TempBuf)->bNbrPorts; // 保存HUB的端口数量
				// 		if (RootHubDev[index].GpVar > 4)
				// 			RootHubDev[index].GpVar = 4; // 因为定义结构DevOnHubPort时人为假定每个HUB不超过4个端口
				// 										 //						if ( ((PHUBDescr)TempBuf) -> wHubCharacteristics[0] & 0x04 ) printf("带有集线器的复合设备\n");
				// 										 //						else printf("单一的集线器产品\n");
				// 		s = SetUsbConfig(cfg);			 // 设置USB设备配置
				// 		if (s == USB_INT_SUCCESS)
				// 		{
				// 			//							需保存端点信息以便主程序进行USB传输,本来中断端点可用于HUB事件通知,但本程序使用查询状态控制传输代替
				// 			//							给HUB各端口上电,查询各端口状态,初始化有设备连接的HUB端口,初始化设备
				// 			for (i = 1; i <= RootHubDev[index].GpVar; i++)
				// 			{																   // 给HUB各端口都上电
				// 				DevOnHubPort[index][i - 1].DeviceStatus = ROOT_DEV_DISCONNECT; // 清外部HUB端口上设备的状态
				// 				s = SetPortFeature(i, PORT_POWER);
				// 				if (s != USB_INT_SUCCESS)
				// 					printf("Ext-HUB Port%01d# power on error\n", (uint16_t)i); // 端口上电失败
				// 			}
				// 			RootHubDev[index].DeviceStatus = ROOT_DEV_SUCCESS;
				// 			SetUsbSpeed(true); // 默认为全速
				// 			return (DEV_HUB);  /* HUB初始化成功 */
				// 		}
				// 	}
				// }
				// else
				// {						   // 可以进一步分析
				// 	s = SetUsbConfig(cfg); // 设置USB设备配置
				// 	if (s == USB_INT_SUCCESS)
				// 	{
				// 		//						需保存端点信息以便主程序进行USB传输
				// 		RootHubDev[index].DeviceStatus = ROOT_DEV_SUCCESS;
				// 		SetUsbSpeed(true);	// 默认为全速
				// 		return (DEV_UNKNOWN); /* 未知设备初始化成功 */
				// 	}
				// }
			}
		}
	}
	printf("InitDevice Error = %02X\n", (uint16_t)s);
	RootHubDev[index].DeviceStatus = ROOT_DEV_FAILED;
	SetUsbSpeed(true); // 默认为全速
	return (DEV_ERROR);
}

uint8_t HubPortEnum(uint8_t index) // 枚举指定ROOT-HUB端口上的外部HUB集线器的各个端口,检查各端口有无连接或移除事件
{
	uint8_t i, s;
	//	printf( "Enum external HUB port\n" );
	for (i = 1; i <= RootHubDev[index].GpVar; i++)
	{							 // 查询集线器的端口是否有变化
		SelectHubPort(index, 0); // 选择操作指定的ROOT-HUB端口,设置当前USB速度以及被操作设备的USB地址
		s = GetPortStatus(i);	// 获取端口状态
		if (s != USB_INT_SUCCESS)
			return (s); // 可能是该HUB断开了
		if ((TempBuf[0] & 0x01) && (TempBuf[2] & 0x01))
		{																  // 发现有设备连接
			DevOnHubPort[index][i - 1].DeviceStatus = ROOT_DEV_CONNECTED; // 有设备连接
			DevOnHubPort[index][i - 1].DeviceAddress = 0;
			s = GetPortStatus(i); // 获取端口状态
			if (s != USB_INT_SUCCESS)
				return (s);														// 可能是该HUB断开了
			DevOnHubPort[index][i - 1].DeviceSpeed = TempBuf[1] & 0x02 ? 0 : 1; // 低速还是全速
			if (DevOnHubPort[index][i - 1].DeviceSpeed)
				printf("Found full speed device on port %01d\n", (uint16_t)i);
			else
				printf("Found low speed device on port %01d\n", (uint16_t)i);
			mDelaymS(200);					   // 等待设备上电稳定
			s = SetPortFeature(i, PORT_RESET); // 对有设备连接的端口复位
			if (s != USB_INT_SUCCESS)
				return (s); // 可能是该HUB断开了
			printf("Reset port and then wait in\n");
			do
			{ // 查询复位端口,直到复位完成,把完成后的状态显示出来
				mDelaymS(1);
				s = GetPortStatus(i);
				if (s != USB_INT_SUCCESS)
					return (s);			 // 可能是该HUB断开了
			} while (TempBuf[0] & 0x10); // 端口正在复位则等待
			mDelaymS(100);
			//			s = ClearPortFeature( i, C_PORT_RESET ); // 清除复位完成标志
			//			s = SetPortFeature( i, PORT_ENABLE );  // 启用HUB端口
			s = ClearPortFeature(i, C_PORT_CONNECTION); // 清除连接或移除变化标志
			if (s != USB_INT_SUCCESS)
				return (s);
			s = GetPortStatus(i); // 再读取状态,复查设备是否还在
			if (s != USB_INT_SUCCESS)
				return (s);
			if ((TempBuf[0] & 0x01) == 0)
				DevOnHubPort[index][i - 1].DeviceStatus = ROOT_DEV_DISCONNECT; // 设备不在了
			s = Level2DevEnum(index, i);
			DevOnHubPort[index][i - 1].DeviceType = s; // 保存设备类型
													   //			if ( s == DEV_ERROR ) {  // 操作失败的端口应该禁止掉
													   //				SelectHubPort( index, 0 );  // 选择操作指定的ROOT-HUB端口,设置当前USB速度以及被操作设备的USB地址
													   //				s = ClearPortFeature( i, PORT_ENABLE );  // 禁止HUB端口
													   //			}
			SetUsbSpeed(true);						   // 默认为全速
		}
		else if ((TempBuf[0] & 0x01) == 0)
		{																   // 设备已经断开
			DevOnHubPort[index][i - 1].DeviceStatus = ROOT_DEV_DISCONNECT; // 有设备连接
			if (TempBuf[2] & 0x01)
				ClearPortFeature(i, C_PORT_CONNECTION); // 清除移除变化标志
		}
	}
	return (USB_INT_SUCCESS); // 返回操作成功
}

uint8_t Level2DevEnum(uint8_t HubIndex, uint8_t PortIndex) // 初始化枚举外部HUB后的二级USB设备
{
	uint8_t i, s, cfg, dv_cls, if_cls;
	printf("Enum dev @ExtHub-port%01d ", (uint16_t)PortIndex);
	printf("@RootHub%01d\n", (uint16_t)HubIndex);
	if (PortIndex == 0)
		return (DEV_ERROR);
	SelectHubPort(HubIndex, PortIndex); // 选择操作指定的ROOT-HUB端口的外部HUB的指定端口,选择速度
	printf("GetDeviceDescr: ");
	s = GetDeviceDescr(TempBuf); // 获取设备描述符
	if (s != USB_INT_SUCCESS)
		return (DEV_ERROR);
	dv_cls = ((PUSB_DEV_DESCR)TempBuf)->bDeviceClass; // 设备类代码
	cfg = ((HubIndex + 1) << 4) + PortIndex;		  // 计算出一个USB地址,避免地址重叠
	s = SetUsbAddress(cfg);							  // 设置USB设备地址
	if (s != USB_INT_SUCCESS)
		return (DEV_ERROR);
	DevOnHubPort[HubIndex][PortIndex - 1].DeviceAddress = cfg; // 保存分配的USB地址
	printf("GetConfigDescr: ");
	s = GetConfigDescr(TempBuf); // 获取配置描述符
	if (s != USB_INT_SUCCESS)
		return (DEV_ERROR);
	cfg = ((PUSB_CFG_DESCR)TempBuf)->bConfigurationValue;
	for (i = 0; i < ((PUSB_CFG_DESCR)TempBuf)->wTotalLengthL; i++)
		printf("0x%02X ", (uint16_t)(TempBuf[i]));
	printf("\n");
	/* 分析配置描述符，获取端点数据/各端点地址/各端点大小等，更新变量endp_addr和endp_size等 */
	if_cls = ((PUSB_CFG_DESCR_LONG)TempBuf)->itf_descr.bInterfaceClass; // 接口类代码
	if (dv_cls == 0x00 && if_cls == 0x08)
	{						   // 是USB存储类设备,基本上确认是U盘
		s = SetUsbConfig(cfg); // 设置USB设备配置
		if (s == USB_INT_SUCCESS)
		{
			DevOnHubPort[HubIndex][PortIndex - 1].DeviceStatus = ROOT_DEV_SUCCESS;
			//			SetUsbSpeed( true );  // 默认为全速
			printf("USB-Disk Ready\n");
			return (DEV_DISK); /* U盘初始化成功 */
		}
	}
	else if (dv_cls == 0x00 && if_cls == 0x03 && ((PUSB_CFG_DESCR_LONG)TempBuf)->itf_descr.bInterfaceSubClass <= 0x01)
	{						   // 是HID类设备,键盘/鼠标等
		s = SetUsbConfig(cfg); // 设置USB设备配置
		if (s == USB_INT_SUCCESS)
		{
			//			需保存端点信息以便主程序进行USB传输
			s = AnalyzeHidIntEndp();						 // 从描述符中分析出HID中断端点的地址
			DevOnHubPort[HubIndex][PortIndex - 1].GpVar = s; // 保存中断端点的地址,位7用于同步标志位,清0
			DevOnHubPort[HubIndex][PortIndex - 1].DeviceStatus = ROOT_DEV_SUCCESS;
			SetUsbSpeed(true); // 默认为全速
			s = ((PUSB_CFG_DESCR_LONG)TempBuf)->itf_descr.bInterfaceProtocol;
			if (s == 1)
			{
				//				进一步初始化,例如设备键盘指示灯LED等
				printf("USB-Keyboard Ready\n");
				return (DEV_KEYBOARD); /* 键盘初始化成功 */
			}
			else if (s == 2)
			{
				//				为了以后查询鼠标状态,应该分析描述符,取得中断端口的地址,长度等信息
				printf("USB-Mouse Ready\n");
				return (DEV_MOUSE); /* 鼠标初始化成功 */
			}
		}
	}
	else if (dv_cls == 0x09)
	{														// 是HUB类设备,集线器等
		printf("This program don't support Level 2 HUB\n"); // 需要支持多级HUB级联请参考本程序进行扩展
	}
	else
	{						   // 可以进一步分析
		s = SetUsbConfig(cfg); // 设置USB设备配置
		if (s == USB_INT_SUCCESS)
		{
			//			需保存端点信息以便主程序进行USB传输
			DevOnHubPort[HubIndex][PortIndex - 1].DeviceStatus = ROOT_DEV_SUCCESS;
			SetUsbSpeed(true);	// 默认为全速
			return (DEV_UNKNOWN); /* 未知设备初始化成功 */
		}
	}
	printf("InitDevice Error = %02X\n", (uint16_t)s);
	DevOnHubPort[HubIndex][PortIndex - 1].DeviceStatus = ROOT_DEV_FAILED;
	SetUsbSpeed(true); // 默认为全速
	return (DEV_ERROR);
}

uint8_t SearchRootHubPort(uint8_t type) // 搜索指定类型的设备所在的端口号,输出端口号为0xFF则未搜索到
{										// 当然也可以根据USB的厂商VID产品PID进行搜索(事先要记录各设备的VID和PID),以及指定搜索序号
	uint8_t i;
	for (i = 0; i < 3; i++)
	{ // 现时搜索可以避免设备中途拔出而某些信息未及时更新的问题
		if (RootHubDev[i].DeviceType == type && RootHubDev[i].DeviceStatus >= ROOT_DEV_SUCCESS)
			return (i); // 类型匹配且枚举成功
	}
	return (0xFF);
}

uint16_t SearchAllHubPort(uint8_t type) // 在ROOT-HUB以及外部HUB各端口上搜索指定类型的设备所在的端口号,输出端口号为0xFFFF则未搜索到
{										// 输出高8位为ROOT-HUB端口号,低8位为外部HUB的端口号,低8位为0则设备直接在ROOT-HUB端口上
										// 当然也可以根据USB的厂商VID产品PID进行搜索(事先要记录各设备的VID和PID),以及指定搜索序号
	uint8_t i, port;
	i = SearchRootHubPort(type); // 搜索指定类型的设备所在的端口号
	if (i != 0xFF)
		return ((uint16_t)i << 8); // 在ROOT-HUB端口上
	return (0xFFFF);
}

void ch374u_init(void)
{

	ch374u_hal_init();
}

void NewDeviceEnum(void)
{
	uint8_t device_count = 0, s = 0;
	if (NewDevCount)
	{				   // 有新的USB设备
		mDelaymS(200); // 由于USB设备刚插入尚未稳定，故等待USB设备数百毫秒，消除插拔抖动
		NewDevCount = 0;
		for (device_count = 0; device_count < 3; device_count++)
		{
			if (RootHubDev[device_count].DeviceStatus == ROOT_DEV_CONNECTED)
			{											 // 刚插入设备尚未初始化
				s = InitDevice(device_count);			 // 初始化/枚举指定HUB端口的USB设备
				RootHubDev[device_count].DeviceType = s; // 保存设备类型
			}
		}
	}
}

void ch374u_loop(void)
{
	uint8_t i = 0, s = 0, n = 0;
	uint8_t count = 0;
	uint16_t loc = 0;
	uint8_t inter_flag_reg = 0;

	printf("Start CH374U Host\n");
	NewDevCount = 0;
	for (n = 0; n < 3; n++)
		RootHubDev[n].DeviceStatus = ROOT_DEV_DISCONNECT; // 清空
	count = 0;

	Init374Host(); // 初始化USB主机

	printf("Wait Device In\n");

	while (1)
	{
		if (Query374Interrupt(&inter_flag_reg) == true)
		{
			HostDetectInterrupt(inter_flag_reg);
		}

		NewDeviceEnum();

		mDelaymS(20); // 模拟单片机做其它事
		count++;
		if(count >= 100)
		{
			count = 0;
		}

		switch (count)
		{									  // 模拟主观请求,对某USB设备进行操作
		case 13:							  // 用定时模拟主观需求,需要操作U盘,请参考CH374LIB\EXAM14\CH374HFT.C程序
			loc = SearchAllHubPort(DEV_ADB); // 在ROOT-HUB以及外部HUB各端口上搜索指定类型的设备所在的端口号
			if (loc != 0xFFFF)
			{ // 找到了
				n = loc >> 8;
				loc &= 0xFF;
				printf("Access ADB %02X %02X\n",n,loc);
				SelectHubPort(n, loc); // 选择操作指定的ROOT-HUB端口,设置当前USB速度以及被操作设备的USB地址
									   //					对U盘进行操作,调用CH374LIB或者HostCtrlTransfer374,HostTransact374等
				SetUsbSpeed(true);	 // 默认为全速

				SetHostUsbAddr(0x03); // 设置USB主机当前操作的USB设备地址

				uint8_t len,buffer2[1024];
				uint8_t bufferA[] = {0x43,0x4e,0x58,0x4e,0x0,00,0x00,0x00,0x01,0x00,0x10,0x00,0x00,0x07,0x00,0x00,0x00,0x32,0x02,0x00,0x00,0xbc,0xb1,0xa7,0xb1};
				uint8_t bufferB[] = {0x68 ,0x6F ,0x73 ,0x74 ,0x3A ,0x3A ,0x66 ,0x65 ,0x61 ,0x74 ,0x75 ,0x72 ,0x65 ,0x73 ,0x3D ,0x63 ,0x6D ,0x64 ,0x2C ,0x73 ,0x68 ,0x65 ,0x6C ,0x6C ,0x5F ,0x76 ,0x32 };
				uint8_t buffer[] = {0x4f,0x50,0x45,0x4e,0x21,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x52,0x02,0x00,0x00,0xb0,0xaf,0xba,0xb1};
				len = sizeof(bufferA);
				Write374Block( RAM_HOST_TRAN, len, bufferA );
				Write374Byte( REG_USB_LENGTH, len );
				s = WaitHostTransact374( 0x03, DEF_USB_PID_OUT, false, 1000 );
				if (s == USB_INT_SUCCESS)
				{			
					printf("Success\r\n");
					len = sizeof(bufferB);
					Write374Block( RAM_HOST_TRAN, len, bufferB);
					Write374Byte( REG_USB_LENGTH, len );
					mDelaymS(100);
					s = WaitHostTransact374( 0x03, DEF_USB_PID_OUT, true, 1000 );
					if (s == USB_INT_SUCCESS)
					{		
						printf("Success\r\n");	
						s = WaitHostTransact374( 0x4, DEF_USB_PID_IN, false, 1000 );
						if (s == USB_INT_SUCCESS)
						{
							len = Read374Byte( REG_USB_LENGTH );
							printf("Success %d\r\n",len);
							Read374Block( RAM_HOST_RECV, len, buffer2 );
							printf_byte(buffer2,len);	
						}else{
							printf("Fail\r\n");
						}
					}else{
						printf("Fail\r\n");
					}
				}else{
					printf("Fail\r\n");
				}



				// len = out_endp_size;
				// Write374Block( RAM_HOST_TRAN, len, buf );
				// Write374Byte( REG_USB_LENGTH, len );
				// s = WaitHostTransact374( out_endp_addr, DEF_USB_PID_OUT, TRUE, 1000 );
				// s = WaitHostTransact374( in_endp_addr, DEF_USB_PID_IN, TRUE, 1000 );
				// len = Read374Byte( REG_USB_LENGTH );
				// Read374Block( RAM_HOST_RECV, len, buf );
			}
			break;
		case 17: // 用定时模拟主观需求,需要操作鼠标
			loc = SearchAllHubPort(DEV_MOUSE); // 在ROOT-HUB以及外部HUB各端口上搜索指定类型的设备所在的端口号
			if (loc != 0xFFFF)
			{ // 找到了,如果有两个MOUSE如何处理?
				n = loc >> 8;
				loc &= 0xFF;
				//printf( "Query Mouse\n" );
				SelectHubPort(n, loc);											// 选择操作指定的ROOT-HUB端口,设置当前USB速度以及被操作设备的USB地址
				i = loc ? DevOnHubPort[n][loc - 1].GpVar : RootHubDev[n].GpVar; // 中断端点的地址,位7用于同步标志位
				if (i & 0x7F)
				{ // 端点有效

					s = HostTransact374((i & 0x7F), DEF_USB_PID_IN, (i & 0x80)); // CH374传输事务,获取数据
					if (s == USB_INT_SUCCESS)
					{
						i ^= 0x80; // 同步标志翻转
						if (loc)
						{
							DevOnHubPort[n][loc - 1].GpVar = i; // 保存同步标志位
						}
						else
						{
							RootHubDev[n].GpVar = i;
						}

						i = Read374Byte(REG_USB_LENGTH); // 接收到的数据长度
						if (i)
						{
							Read374Block(RAM_HOST_RECV, i, TempBuf); // 取出数据并打印
							printf("Mouse data: ");
							for (s = 0; s < i; s++)
								printf("0x%02X ", *(TempBuf + s));
							printf("\n");
						}
					}
					else if (s != (0x20 | USB_INT_RET_NAK))
						printf("Mouse error %02x\n", (uint16_t)s); // 可能是断开了
				}
				else
					printf("Mouse no interrupt endpoint\n");
				SetUsbSpeed(true); // 默认为全速
			}
			break;
		}
	}
}