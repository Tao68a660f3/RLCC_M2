#include <STC89C5xRC.H>
#include <intrins.h>


#include "LCD_14432.h"

sbit LCD14432_CS = P0 ^ 0;
sbit LCD14432_SID = P0 ^ 1;
sbit LCD14432_SCLK = P0 ^ 2;


void LCD_Delay1ms(void)	//@11.0592MHz
{
	unsigned char data i, j;

	_nop_();
	i = 2;
	j = 199;
	do
	{
		while (--j);
	} while (--i);
}

void LCD14432_Init()
{
	LCD14432_SendData(0, 0, 0x30);	//基本指令集
	//LCD14432_SendData(0, 0, 0x34);	//扩展指令集
	LCD14432_SendData(0, 0, 0x06);	//起始点设定：光标右移
	LCD14432_SendData(0, 0, 0x01);	//清除显示DDROM
	LCD14432_SendData(0, 0, 0x0c);	//显示状态开关：整体显示开，光标显示关，光标显示反白关
	LCD14432_SendData(0, 0, 0x02);	//地址归零
}

void LCD14432_SendByte(unsigned char Byte)
{
	unsigned char i;
	for(i = 0; i < 8; i++)
	{
		if(Byte & (0x80 >> i))	LCD14432_SID = 1;
		else	LCD14432_SID = 0;
		LCD14432_SCLK = 0;
		LCD14432_SCLK = 1;
	}
}

unsigned char LCD14432_ReadByte()
{
	unsigned char i, Byte = 0x00;
    for(i = 0; i < 8; i++) {
        LCD14432_SCLK = 0;          // 下降沿（设备准备下一位）
        LCD14432_SCLK = 1;          // 上升沿（设备输出数据）
        Byte <<= 1;                 // 左移，准备接收下一位
        if(LCD14432_SID) Byte |= 1; // 读取当前位
    }
	return Byte;
}

void LCD14432_SendData(unsigned char RS, RW, Data)
{
	unsigned char startByte = 0xf8;
	if(RW) startByte |= 0x04;
	if(RS) startByte |= 0x02;
	
	LCD14432_CS = 0;
	LCD14432_SCLK = 0;
	
	LCD14432_CS = 1;
	
	LCD14432_SendByte(startByte);
	LCD14432_SendByte(Data & 0xf0);
	LCD14432_SendByte((Data << 4) & 0xf0);
	
	LCD14432_CS = 0;
	LCD14432_SCLK = 0;
}

unsigned char LCD14432_ReadData(unsigned char RS, RW)
{
	unsigned char Data = 0x00;
	unsigned char startByte = 0xf8;
	if(RW) startByte |= 0x04;
	if(RS) startByte |= 0x02;
	
	LCD14432_CS = 0;
	LCD14432_SCLK = 0;
	
	LCD14432_CS = 1;
	
	LCD14432_SendByte(startByte);
	Data |= LCD14432_ReadByte();
	Data |= LCD14432_ReadByte() >> 4;
	
	LCD14432_CS = 0;
	LCD14432_SCLK = 0;
	
	return Data;
}

void LCD14432_WriteCommand(unsigned char CMD)
{
	LCD14432_SendData(0, 0, CMD);
	LCD_Delay1ms();
}

void LCD14432_WriteData(unsigned char Data)
{
	LCD14432_SendData(1, 0, Data);
}

unsigned char LCD14432_GetData()
{
	unsigned char Data;
	Data = LCD14432_ReadData(1, 1);
	return Data;
}

int LCD14432_Pow(int x, y)
{
	unsigned char i;
	int res = 1;
	for(i = 0; i < y; i++) res *= x;
	return res;
}

void _WriteStringWithAutoSpace(const char *str)
{
    int i = 0;
    int asciiCount = 0; // 当前连续ASCII字符数
    
    while (str[i] != '\0' && i < 18)
    {
        if ((unsigned char)str[i] < 0x80)
        { // ASCII字符（单字节）
            LCD14432_WriteData(str[i]);
            asciiCount++;
            i++;
        }
        else
        { // 汉字（双字节）
            // 先检查之前的ASCII序列是否需要补空格
            if (asciiCount % 2 != 0)
            {
                LCD14432_WriteData(' ');
            }
            asciiCount = 0; // 重置计数器
            
            // 写入汉字（2字节）
            LCD14432_WriteData(str[i]);
            LCD14432_WriteData(str[i+1]);
            i += 2;
        }
    }
    
    // 字符串结束时，检查剩余的ASCII计数
    if (asciiCount % 2 != 0)
    {
        LCD14432_WriteData(' ');
    }
}

void LCD14432_ShowString(unsigned char Line, Col, unsigned char * str)
{
	unsigned char addr, i;
	if(Line == 1) addr = 0x80;
	else addr = 0x90;
	addr = addr + Col % 10 - 1;
	LCD14432_WriteCommand(addr); //设定DDRAM地址
	_WriteStringWithAutoSpace(str);
}

void LCD14432_ShowNum(unsigned char Line, Col, unsigned int Num, unsigned char Len)
{
	unsigned char i, addr;
	if(Line == 1) addr = 0x80;
	else addr = 0x90;
	
	addr = addr + Col % 10 - 1;
	LCD14432_WriteCommand(addr); //设定DDRAM地址
	for(i = Len; i > 0; i--)
	{
		LCD14432_WriteData(Num / LCD14432_Pow(10, i - 1) % 10 + '0');
	}
}

void LCD14432_ShowSignedNum(unsigned char Line, Col, int Num, unsigned char Len)
{
	unsigned char i, addr;
	unsigned int N;
	if(Line == 1) addr = 0x80;
	else addr = 0x90;
	addr = addr + Col % 10 - 1;
	LCD14432_WriteCommand(addr); //设定DDRAM地址
	if(Num >= 0)
	{
		LCD14432_WriteData('+');
		N = Num;
	}
	else
	{
		LCD14432_WriteData('-');
		N = -Num;
	}
	for(i = Len; i > 0; i--)
	{
		LCD14432_WriteData(N / LCD14432_Pow(10, i - 1) % 10 + '0');
	}
}

void LCD14432_ShowHexNum(unsigned char Line, Col, unsigned int Num, unsigned char Len)
{
	unsigned char i, addr;
	unsigned char N;
	if(Line == 1) addr = 0x80;
	else addr = 0x90;
	addr = addr + Col % 10 - 1;
	LCD14432_WriteCommand(addr); //设定DDRAM地址
	for(i = Len; i > 0; i--)
	{
		N = Num / LCD14432_Pow(16, i - 1) % 16;
		if(N < 10) LCD14432_WriteData(N + '0');
		else LCD14432_WriteData(N + 'A' - 10);
	}
}





















