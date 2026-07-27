#ifndef __LCD_14432_H__
#define __LCD_14432_H__

void LCD14432_Init();
void LCD14432_SendByte(unsigned char Byte);
unsigned char LCD14432_ReadByte();
void LCD14432_SendData(unsigned char RS, RW, Data);
unsigned char LCD14432_ReadData(unsigned char RS, RW);
void LCD14432_WriteCommand(unsigned char CMD);
void LCD14432_WriteData(unsigned char Data);
unsigned char LCD14432_GetData();
void LCD14432_ShowString(unsigned char Line, Col, unsigned char * String);
void LCD14432_ShowNum(unsigned char Line, Col, unsigned int Num, unsigned char Len);
void LCD14432_ShowSignedNum(unsigned char Line, Col, int Num, unsigned char Len);
void LCD14432_ShowHexNum(unsigned char Line, Col, unsigned int Num, unsigned char Len);



void test(unsigned char addr, unsigned char * hanzi, unsigned char count);







#endif
