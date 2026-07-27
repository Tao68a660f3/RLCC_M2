#ifndef __LCD_14432_TEXTMODEDRIVER_H__
#define __LCD_14432_TEXTMODEDRIVER_H__

void LCD14432_Init();
void LCD14432_SendByte(unsigned char Byte);
void LCD14432_SendData(unsigned char RS, RW, Data);
void LCD14432_WriteCommand(unsigned char CMD);
void LCD14432_WriteData(unsigned char Data);
unsigned char LCD14432_SetCursorAddr(unsigned char Line, Col);
void LCD14432_ShowString(unsigned char Line, Col, unsigned char * str);
void LCD14432_ShowNum(unsigned char Line, Col, unsigned int Num, unsigned char Len);
void LCD14432_ShowSignedNum(unsigned char Line, Col, int Num, unsigned char Len);
void LCD14432_ShowFloatNum(unsigned char Line, unsigned char Col, float Num, unsigned char DotLen);
void LCD14432_ShowHexNum(unsigned char Line, Col, unsigned int Num, unsigned char Len);


#endif
