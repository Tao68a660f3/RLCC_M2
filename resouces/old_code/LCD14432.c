#include "LCD14432.h"
//#include <string.h>

extern SPI_HandleTypeDef hspi1;

static LCD_Queue_t lcdQueue;
static LCD_Scroll_t lcdScroll =
{ 0, 150, 0, 0 };

/**
 * @brief 硬件层：发送 3 字节串行包
 */
static void LCD_HardwareSend(uint8_t RS, uint8_t Data)
{
	uint8_t packet[3];
	// 起始字节：11111 + RW(0) + RS + 0
	packet[0] = RS ? 0xFA : 0xF8;
	packet[1] = Data & 0xF0;
	packet[2] = (Data << 4) & 0xF0;

	// CS 高电平有效
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
	// 调用 HAL 库阻塞发送
	HAL_SPI_Transmit(&hspi1, packet, 3, 10);
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
}

/**
 * @brief 基础异步写入接口 (供内部显示函数调用)
 * @param RS: 0为命令, 1为数据
 */
static void LCD_Write_Async(uint8_t RS, uint8_t cmd_or_data)
{
	uint16_t next_tail = (lcdQueue.tail + 1) % LCD_BUF_SIZE;

	// 缓冲区满：阻塞等待直到腾出空位
	while (next_tail == lcdQueue.head)
	{
		LCD_Task_Handler();
	}

	lcdQueue.data[lcdQueue.tail] = cmd_or_data;
	lcdQueue.is_rs[lcdQueue.tail] = RS;
	lcdQueue.tail = next_tail;
}

/**
 * @brief 异步任务处理器，可以放在 main 的 while(1) 中轮询
 * @note  负责检查时间戳并消化缓冲区里的数据
 */
void LCD_Task_Handler(void)
{
	// 指令 / 数据队列处理逻辑
	if (lcdQueue.head != lcdQueue.tail)
	{
		uint8_t rs = lcdQueue.is_rs[lcdQueue.head];
		uint8_t data = lcdQueue.data[lcdQueue.head];
		uint32_t wait_time;
		if (rs == 0)
		{
			// 如果是指令
			if (data == 0x01)
				wait_time = 5; // 清屏给 5ms
			else
				wait_time = 2;              // 其他指令（如设坐标）给 2ms
		}
		else
		{
			// 如果是纯数据
			wait_time = 1;                   // 数据保持 1ms
		}
		if (HAL_GetTick() - lcdQueue.last_tick >= wait_time)
		{
			LCD_HardwareSend(rs, data);
			lcdQueue.last_tick = HAL_GetTick();
			lcdQueue.head = (lcdQueue.head + 1) % LCD_BUF_SIZE;
		}
	}
	// 自动滚动逻辑：只有在队列为空时才插入滚动指令，防止画面撕裂
	else if (lcdScroll.scroll_en)
	{
		if (HAL_GetTick() - lcdScroll.last_scroll_tick
				>= lcdScroll.scroll_interval)
		{
			lcdScroll.last_scroll_tick = HAL_GetTick();
			lcdScroll.scroll_offset = (lcdScroll.scroll_offset + 1)
					% SCREEN_ROLL_HEIGHT;

			// 直接调用异步写入，将滚动指令排入队列
			LCD_Write_Async(0, 0x34);
			LCD_Write_Async(0, 0x03);
			LCD_Write_Async(0, 0x40 + lcdScroll.scroll_offset);
			LCD_Write_Async(0, 0x30);
		}
	}
}

/**
 * @note  显示屏初始化
 */
void LCD_Init(void)
{
	// PSB 引脚如果接了 IO，需要拉低进入串口模式
	// HAL_GPIO_WritePin(LCD_PSB_GPIO_Port, LCD_PSB_Pin, GPIO_PIN_RESET);
	// HAL_Delay(10);

	// 直接使用底层发送，不走异步队列
	LCD_HardwareSend(0, 0x30); // 基本指令集
	HAL_Delay(1);
	LCD_HardwareSend(0, 0x0C); // 显示开
	HAL_Delay(1);
	LCD_HardwareSend(0, 0x01); // 清屏
	HAL_Delay(10);             // 清屏需要更长时间

	// 初始化最后，更新一下时间戳，防止后续异步任务立即触发
	lcdQueue.last_tick = HAL_GetTick();
}

/**
 * @note  清除显示
 */
void LCD_Clear(void)
{
	LCD_Write_Async(0, 0x01);
}

/**
 * @brief  计算x的y次幂（整数运算）
 * @param  x: 底数
 * @param  y: 指数
 * @retval x^y的结果
 * @note   用于浮点数显示时的位数计算
 */
static uint16_t LCD_Pow(uint16_t x, uint16_t y)
{
	uint8_t i;
	uint16_t res = 1;
	for (i = 0; i < y; i++)
		res *= x;  // 循环乘法实现幂运算
	return res;
}

/**
 * @brief  计算LCD14432的DDRAM地址
 * @param  line: 行号（1或2）
 * @param  col: 列号（1~9）
 * @retval DDRAM地址
 * @note   - 第一行基地址0x80，第二行0x90
 *         - 列号超出范围时自动取模
 */
void LCD_SetCursorPos(uint8_t line, uint8_t col)
{
	uint8_t addr;
	if (line == 1)
		addr = 0x80; // 第一行起始地址
	else
		addr = 0x90; // 第二行起始地址
	addr = addr + col % 10 - 1;    // 列地址偏移（限制0-8）
	LCD_Write_Async(0, addr); //设定DDRAM地址
}

/**
 * @brief  自动处理中西文混排的字符串显示（自动对齐双字节字符）
 * @param  str: 要显示的字符串（GB2312编码）
 * @retval 是否大于36（超过一屏）
 * @note   - ASCII字符单字节显示，汉字双字节显示
 *         - 当ASCII字符数为奇数时自动补空格对齐
 *         - 最大显示长度限制为72字节（防止溢出）
 */
uint8_t LCD_ShowStringWithAutoSpace(const char *str, uint8_t HAlign)
{
	uint8_t i = 0, j = 0;
	uint16_t asciiCount = 0; // 当前连续ASCII字符数

//	清除显示并回到第1行第1列，这里不用
//	LCD_Write_Async(0, 0x01);
//	LCD_Write_Async(0, 0x80);

	while (str[i] != '\0' && i < 72)
	{
		if ((uint8_t) str[i] < 0x80)
		{ // ASCII字符（单字节）
			LCD_Write_Async(1, str[i]);
			asciiCount++;
			i++;
			j++;
			if (j % 18 == 0)
			{
				uint16_t n = j / 18;
				LCD_Write_Async(0, 0x80 | ((n & 0x0F) << 4));
			}
		}
		else
		{ // 汉字（双字节）
		  // 若前面有奇数个ASCII字符，补空格对齐
			if (asciiCount % 2 != 0)
			{
				LCD_Write_Async(1, ' ');
				j++;
				if (j % 18 == 0)
				{
					uint16_t n = j / 18;
					LCD_Write_Async(0, 0x80 | ((n & 0x0F) << 4));
				}
			}
			asciiCount = 0; // 重置计数器
			// 写入汉字（连续2字节）
			LCD_Write_Async(1, str[i]);
			LCD_Write_Async(1, str[i + 1]);
			i += 2;
			j += 2;
			if (j % 18 == 0)
			{
				uint16_t n = j / 18;
				LCD_Write_Async(0, 0x80 | ((n & 0x0F) << 4));
			}
		}
	}
	// 字符串结束时补空格对齐（如果剩余ASCII为奇数）
	if (asciiCount % 2 != 0)
	{
		LCD_Write_Async(1, ' ');
		j++;
	}
	if (j <= 18 && HAlign)
	{
		LCD_Scroll_Reset(SCREEN_ROLL_HEIGHT - 8);
	}
	return j > 36;
}

/**
 * @brief  自动处理中西文混排的字符串显示（自动对齐双字节字符）
 * @param  line: 行号（1或2）
 * @param  col: 列号（1~9）
 * @param  str: 要显示的字符串（GB2312编码）
 * @note   - ASCII字符单字节显示，汉字双字节显示
 *         - 当ASCII字符数为奇数时自动补空格对齐
 *         - 最大显示长度限制为72字节（防止溢出，由被调用的LCD_ShowStringWithAutoSpace实现）
 */
void LCD_ShowString(uint8_t line, uint8_t col, uint8_t HAlign, const char *str)
{
	LCD_SetCursorPos(line, col);
	LCD_ShowStringWithAutoSpace(str, HAlign);
}

/**
 * @brief  显示无符号整数
 * @param  line: 行号（1或2）
 * @param  col: 列号（1~9）
 * @param  num: 要显示的数值
 * @param  len: 显示位数（1~4）
 * @retval 无
 * @note   - 不足位数时高位补零
 *         - 超过显示范围时可能显示错误
 */
void LCD_ShowNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len)
{
	uint8_t i;
	LCD_SetCursorPos(line, col);
	for (i = len; i > 0; i--)
	{
		// 逐位提取并转换为ASCII
		LCD_Write_Async(1, num / LCD_Pow(10, i - 1) % 10 + '0');
	}
}

/**
 * @brief  显示有符号整数
 * @param  line: 行号（1或2）
 * @param  col: 列号（1~9）
 * @param  num: 要显示的数值
 * @param  len: 显示位数（不含符号位，1~3）
 * @retval 无
 * @note   - 正数显示"+"号，负数显示"-"号
 *         - 数值范围受Len限制
 */
void LCD_ShowSignedNum(uint8_t line, uint8_t col, int32_t num, uint8_t len)
{
	uint8_t i;
	uint16_t N;
	LCD_SetCursorPos(line, col);
	// 处理符号
	if (num >= 0)
	{
		LCD_Write_Async(1, '+');
		N = num;
	}
	else
	{
		LCD_Write_Async(1, '-');
		N = -num;
	}
	// 显示数字部分
	for (i = len; i > 0; i--)
	{
		LCD_Write_Async(1, N / LCD_Pow(10, i - 1) % 10 + '0');
	}
}

/**
 * @brief  在LCD上显示浮点数
 * @param  line: 行号（1或2）
 * @param  col: 列号（1~9）
 * @param  num: 要显示的浮点数
 * @param  dotLen: 小数位数（1~5）
 * @retval 无
 */
void LCD_ShowFloatNum(uint8_t line, uint8_t col, float num, uint8_t dotLen)
{
	uint8_t i, j, digit_count;
	uint32_t integer_part, decimal_part;
	uint32_t multiplier = 1, temp, divisor;
	LCD_SetCursorPos(line, col);
	if (dotLen > 5)
		dotLen = 5;
	for (i = 0; i < dotLen; i++)
		multiplier *= 10;
	if (num < 0)
	{
		LCD_Write_Async(1, '-');
		num = -num;
	}
	integer_part = (uint32_t) num;
	// 关键修复点：四舍五入计算
	decimal_part =
			(uint32_t) ((num - (float) integer_part) * multiplier + 0.5f);
	// 重点：处理小数进位到整数的情况（例如 0.99 进位变成 1.00）
	if (decimal_part >= multiplier)
	{
		integer_part += 1;
		decimal_part = 0; // 进位后小数部分归零，解决多出 0 的问题
	}
	// 显示整数部分
	if (integer_part == 0)
	{
		LCD_Write_Async(1, '0');
	}
	else
	{
		digit_count = 0;
		for (temp = integer_part; temp > 0; temp /= 10)
			digit_count++;
		for (i = digit_count; i > 0; i--)
		{
			divisor = 1;
			for (j = 1; j < i; j++)
				divisor *= 10;
			LCD_Write_Async(1, (integer_part / divisor) % 10 + '0');
		}
	}
	// 显示小数部分
	if (dotLen > 0)
	{
		LCD_Write_Async(1, '.');
		temp = decimal_part;
		digit_count = 0;
		if (temp > 0)
		{
			while (temp > 0)
			{
				temp /= 10;
				digit_count++;
			}
		}
		else
		{
			digit_count = 1; // 即使是 0 也要占位
		}
		// 补足前导零（例如 1.02 中的 0）
		for (i = dotLen; i > digit_count; i--)
		{
			LCD_Write_Async(1, '0');
		}
		// 显示小数非零位
		if (decimal_part > 0)
		{
			for (i = digit_count; i > 0; i--)
			{
				divisor = 1;
				for (j = 1; j < i; j++)
					divisor *= 10;
				LCD_Write_Async(1, (decimal_part / divisor) % 10 + '0');
			}
		}
		else
		{
			// 如果小数部分完全为0，补齐剩余的0
			LCD_Write_Async(1, '0');
			// 注意：上面的循环已经补了 (dotLen-1) 个 0，这里补完最后一个
		}
	}
}

/**
 * @brief  显示十六进制数
 * @param  line: 行号（1或2）
 * @param  col: 列号（1~9）
 * @param  num: 要显示的数值（0x0000~0xFFFF）
 * @param  len: 显示位数（1~4）
 * @retval 无
 * @note   - 字母以大写形式显示（A-F）
 */
void LCD_ShowHexNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len)
{
	uint8_t i;
	uint8_t N;
	LCD_SetCursorPos(line, col);
	for (i = len; i > 0; i--)
	{
		N = num / LCD_Pow(16, i - 1) % 16;
		if (N < 10)
			LCD_Write_Async(1, N + '0');  // 数字0-9
		else
			LCD_Write_Async(1, N + 'A' - 10); // 字母A-F
	}
}

void LCD_Scroll_Start(uint16_t interval_ms)
{
	lcdScroll.scroll_en = 1;
	lcdScroll.scroll_interval = interval_ms;
	lcdScroll.last_scroll_tick = HAL_GetTick();
}

void LCD_Scroll_Stop(void)
{
	lcdScroll.scroll_en = 0;
}

void LCD_Scroll_Reset(uint8_t start_pos)
{
	lcdScroll.scroll_offset = start_pos % SCREEN_ROLL_HEIGHT;
	LCD_Write_Async(0, 0x34);
	LCD_Write_Async(0, 0x03);
	LCD_Write_Async(0, 0x40 + lcdScroll.scroll_offset); // 恢复设定的地址
	LCD_Write_Async(0, 0x30);
}
