#include <STC89C5xRC.H>
#include <intrins.h>


#include "LCD_14432_textModeDriver.h"

sbit LCD14432_CS = P0 ^ 0;
sbit LCD14432_SID = P0 ^ 1;
sbit LCD14432_SCLK = P0 ^ 2;
sbit LCD14432_PSB = P0 ^ 3;


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
	LCD14432_PSB = 0;					//选择串口模式
	LCD14432_SendData(0, 0, 0x30);		//基本指令集
	//LCD14432_SendData(0, 0, 0x34);	//扩展指令集
	LCD14432_SendData(0, 0, 0x06);		//起始点设定：光标右移
	LCD14432_SendData(0, 0, 0x01);		//清除显示DDROM
	LCD14432_SendData(0, 0, 0x0c);		//显示状态开关：整体显示开，光标显示关，光标显示反白关
	LCD14432_SendData(0, 0, 0x02);		//地址归零
}

/**
  * @brief  通过串行接口发送一个字节数据到LCD14432
  * @param  Byte: 要发送的字节数据（8位）
  * @retval 无
  * @note   数据在SCLK上升沿被采样，MSB优先发送
  */
void LCD14432_SendByte(unsigned char Byte)
{
    unsigned char i;
    for(i = 0; i < 8; i++)
    {
        // 逐位发送（从最高位开始）
        if(Byte & (0x80 >> i)) LCD14432_SID = 1;  // 当前位为1
        else                   LCD14432_SID = 0;  // 当前位为0
        
        // 生成时钟信号（下降沿→上升沿）
        LCD14432_SCLK = 0;  // 准备数据
        LCD14432_SCLK = 1;  // 上升沿锁存数据
    }
}

/**
  * @brief  发送数据或命令到LCD14432（底层函数）
  * @param  RS: 寄存器选择（0=命令寄存器，1=数据寄存器）
  * @param  RW: 读写控制（0=写，1=读）
  * @param  Data: 要发送的数据字节
  * @retval 无
  * @note   通信协议：同步头(5x1) + 控制位(RW/RS/0) + 数据分片(高4位+低4位)
  */
void LCD14432_SendData(unsigned char RS, RW, Data)
{
    // 构建起始字节（格式：11111RWRS0）
    unsigned char startByte = 0xf8;  // 基础值（11111000）
    if(RW) startByte |= 0x04;       // 设置RW位（bit2）
    if(RS) startByte |= 0x02;       // 设置RS位（bit1）
    
    // 通信起始序列
    LCD14432_CS = 0;   // 片选使能
    LCD14432_SCLK = 0; // 初始化时钟
    
    LCD14432_CS = 1;   // 开始传输
    
    // 发送数据包
    LCD14432_SendByte(startByte);       // 起始字节
    LCD14432_SendByte(Data & 0xf0);     // 高4位（D7-D4）
    LCD14432_SendByte((Data << 4) & 0xf0); // 低4位（D3-D0）左移后发送
    
    // 通信结束
    LCD14432_CS = 0;   // 片选禁用
    LCD14432_SCLK = 0; // 时钟归零
}

/**
  * @brief  向LCD14432写入命令
  * @param  CMD: 要发送的命令字节
  * @retval 无
  * @note   调用后延时1ms确保命令执行完成
  */
void LCD14432_WriteCommand(unsigned char CMD)
{
    LCD14432_SendData(0, 0, CMD);  // RS=0（命令），RW=0（写）
    LCD_Delay1ms();                // 命令执行延时
}

/**
  * @brief  向LCD14432写入显示数据
  * @param  Data: 要显示的数据字节
  * @retval 无
  */
void LCD14432_WriteData(unsigned char Data)
{
    LCD14432_SendData(1, 0, Data);  // RS=1（数据），RW=0（写）
}

/**
  * @brief  计算x的y次幂（整数运算）
  * @param  x: 底数
  * @param  y: 指数
  * @retval x^y的结果
  * @note   用于浮点数显示时的位数计算
  */
int _LCD14432_Pow(int x, int y)
{
    unsigned char i;
    int res = 1;
    for(i = 0; i < y; i++) res *= x;  // 循环乘法实现幂运算
    return res;
}

/**
  * @brief  自动处理中西文混排的字符串显示（自动对齐双字节字符）
  * @param  str: 要显示的字符串（GB2312编码）
  * @retval 无
  * @note   - ASCII字符单字节显示，汉字双字节显示
  *         - 当ASCII字符数为奇数时自动补空格对齐
  *         - 最大显示长度限制为18字节（防止溢出）
  */
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
            // 若前面有奇数个ASCII字符，补空格对齐
            if (asciiCount % 2 != 0)
            {
                LCD14432_WriteData(' ');
            }
            asciiCount = 0; // 重置计数器
            
            // 写入汉字（连续2字节）
            LCD14432_WriteData(str[i]);
            LCD14432_WriteData(str[i+1]);
            i += 2;
        }
    }
    
    // 字符串结束时补空格对齐（如果剩余ASCII为奇数）
    if (asciiCount % 2 != 0)
    {
        LCD14432_WriteData(' ');
    }
}

/**
  * @brief  计算LCD14432的DDRAM地址
  * @param  Line: 行号（1或2）
  * @param  Col: 列号（1~9）
  * @retval DDRAM地址
  * @note   - 第一行基地址0x80，第二行0x90
  *         - 列号超出范围时自动取模
  */
unsigned char LCD14432_SetCursorAddr(unsigned char Line, Col)
{
    unsigned char addr;
    if(Line == 1) addr = 0x80; // 第一行起始地址
    else          addr = 0x90; // 第二行起始地址
    addr = addr + Col % 10 - 1;    // 列地址偏移（限制0-8）
    return addr;
}

/**
  * @brief  在指定位置显示字符串
  * @param  Line: 行号（1或2）
  * @param  Col: 列号（1~9）
  * @param  str: 要显示的字符串（GB2312编码）
  * @retval 无
  */
void LCD14432_ShowString(unsigned char Line, Col, unsigned char *str)
{
    unsigned char addr;
    addr = LCD14432_SetCursorAddr(Line, Col);
    LCD14432_WriteCommand(addr); // 设置光标位置
    _WriteStringWithAutoSpace(str); // 显示字符串
}

/**
  * @brief  显示无符号整数
  * @param  Line: 行号（1或2）
  * @param  Col: 列号（1~9）
  * @param  Num: 要显示的数值
  * @param  Len: 显示位数（1~4）
  * @retval 无
  * @note   - 不足位数时高位补零
  *         - 超过显示范围时可能显示错误
  */
void LCD14432_ShowNum(unsigned char Line, Col, unsigned int Num, unsigned char Len)
{
    unsigned char i, addr;
    addr = LCD14432_SetCursorAddr(Line, Col);
    LCD14432_WriteCommand(addr); // 设置光标位置
    for(i = Len; i > 0; i--)
    {
        // 逐位提取并转换为ASCII
        LCD14432_WriteData(Num / _LCD14432_Pow(10, i - 1) % 10 + '0');
    }
}

/**
  * @brief  显示有符号整数
  * @param  Line: 行号（1或2）
  * @param  Col: 列号（1~9）
  * @param  Num: 要显示的数值
  * @param  Len: 显示位数（不含符号位，1~3）
  * @retval 无
  * @note   - 正数显示"+"号，负数显示"-"号
  *         - 数值范围受Len限制
  */
void LCD14432_ShowSignedNum(unsigned char Line, Col, int Num, unsigned char Len)
{
    unsigned char i, addr;
    unsigned int N;
    addr = LCD14432_SetCursorAddr(Line, Col);
    LCD14432_WriteCommand(addr); // 设置光标位置
    
    // 处理符号
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
    
    // 显示数字部分
    for(i = Len; i > 0; i--)
    {
        LCD14432_WriteData(N / _LCD14432_Pow(10, i - 1) % 10 + '0');
    }
}

/**
  * @brief  在LCD上显示浮点数
  * @param  Line: 行号（1或2）
  * @param  Col: 列号（1~9）
  * @param  Num: 要显示的浮点数
  * @param  DotLen: 小数位数（1~5）
  * @retval 无
  */
void LCD14432_ShowFloatNum(unsigned char Line, unsigned char Col, float Num, unsigned char DotLen)
{
    unsigned char i, j, digit_count;
    unsigned long integer_part, decimal_part;
    unsigned long multiplier = 1, temp, divisor;
    
    // 设置光标位置（已包含参数检查）
    LCD14432_WriteCommand(LCD14432_SetCursorAddr(Line, Col));
    
    // 参数校验
    if (DotLen > 5) DotLen = 5;  // 限制最大5位小数
    
    // 计算放大系数
    for (i = 0; i < DotLen; i++) {
        multiplier *= 10;
    }
    
    // 处理负数
    if (Num < 0) {
        LCD14432_WriteData('-');
        Num = -Num;
    }
    
    // 分离整数和小数部分
    integer_part = (unsigned long)Num;
    decimal_part = (unsigned long)((Num - integer_part) * multiplier + 0.5); // 四舍五入
    
    // 显示整数部分
    if (integer_part == 0) {
        LCD14432_WriteData('0');
    } else {
        // 计算整数位数
        digit_count = 0;
        for (temp = integer_part; temp > 0; temp /= 10) {
            digit_count++;
        }
        // 逐位显示
        for (i = digit_count; i > 0; i--) {
            divisor = 1;
            for (j = 1; j < i; j++) {
                divisor *= 10;
            }
            LCD14432_WriteData((integer_part / divisor) % 10 + '0');
        }
    }
    
    // 显示小数部分
    if (DotLen > 0 && decimal_part >= 0) {
        LCD14432_WriteData('.');
        // 计算小数位数（避免前导零）
        digit_count = 0;
        for (temp = decimal_part; temp > 0; temp /= 10) {
            digit_count++;
        }
        // 补零（确保显示指定的小数位数）
        for (i = DotLen; i > digit_count; i--) {
            LCD14432_WriteData('0');
        }
        // 逐位显示
        for (i = digit_count; i > 0; i--) {
            divisor = 1;
            for (j = 1; j < i; j++) {
                divisor *= 10;
            }
            LCD14432_WriteData((decimal_part / divisor) % 10 + '0');
        }
    }
}

/**
  * @brief  显示十六进制数
  * @param  Line: 行号（1或2）
  * @param  Col: 列号（1~9）
  * @param  Num: 要显示的数值（0x0000~0xFFFF）
  * @param  Len: 显示位数（1~4）
  * @retval 无
  * @note   - 字母以大写形式显示（A-F）
  */
void LCD14432_ShowHexNum(unsigned char Line, Col, unsigned int Num, unsigned char Len)
{
    unsigned char i, addr;
    unsigned char N;
    addr = LCD14432_SetCursorAddr(Line, Col);
    LCD14432_WriteCommand(addr); // 设置光标位置
    for(i = Len; i > 0; i--)
    {
        N = Num / _LCD14432_Pow(16, i - 1) % 16;
        if(N < 10) LCD14432_WriteData(N + '0');  // 数字0-9
        else       LCD14432_WriteData(N + 'A' - 10); // 字母A-F
    }
}
