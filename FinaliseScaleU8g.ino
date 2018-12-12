/* Original concept by Olav Kallhovd 2016 - 2017.
*  Breakout with a unique Arduino nano and 0.96 OLED screen by Stephan Gamp, Dec 2019 Version 1.0.
*  CG Scale main components :
* 1 pc load sensor front YZC - 133 2kg
* 1 pc load sensor rear YZC - 133 3kg
* 2 pc HX711 ADC, one for each load sensor(128bit resolution)
* 1 pc Arduino nano for the scale and the display
* 1 pc Oled 0.96" ssd1306 or 16*2 HD44780 LCD 
* 5 push buttons and some resistors
* Changes: Possibility to enter a reference weight with push buttons
*          Autocalibration of Scale after entering reference weight
*		   Steps are guided on display
*		   Values are written and read to/from EEPROM
* Max model weight with sensors above : 4 to 4, 5kg depending on CG location
*
*/


#include <Wire.h>
#include <HX711_ADC.h>
#include <EEPROM.h>
#include <U8glib.h>

// Groesse des EEPROMs
#define EESIZE 1024

//#define OLED_RESET 4
#define TRUE 1
#define FALSE 0

//HX711 constructor (dout pin, sck pin)
HX711_ADC ScaleBack(4, 5);
HX711_ADC ScaleFront(2, 3);

//Adafruit_SSD1306 display(OLED_RESET);
U8GLIB_SSD1306_128X64 u8g(U8G_I2C_OPT_NONE | U8G_I2C_OPT_DEV_0);

//define values for scale
float SW_front=10.0; //ScaleWeight
float SW_back=10.0;
float SF_front = 1070.0; //ScaleFactor
float SF_back = 610.0;
const long DistSupPins = 1450; //calibration value in 1/10mm, between the support pins
const long DistStopSup = 285; //calibration value 1/10mm, projected distance from front wing support point to stopper pin
const long CGoffset = ((DistSupPins / 2) + DistStopSup) * 10;  //offset calaculation as plane is not centrally above support points of scale

//define values for eerpom
int adrSF_front = 0;  //adresses for EEPROM values
int adrSF_back = 5;
int adrTotalWeight = 10;
int adrFirstRun = 15;

//various stuff
byte buttonPin = A6; //pin for input switches
byte batRefPin = A7;// pin to check BAT value    
boolean bBatWarning = FALSE;
int firstRun = 99;
int gewicht = 300;


//values for display with U8g font
int abst = 13;//number of pixels between the lines
int line1 = 10;
int line2 = line1 + 1 * abst;
int line3 = line1 + 2 * abst;
int line4 = line1 + 3 * abst;
int line5 = line1 + 4 * abst;

//define all subroutines
char buttonhit(int value); //function returns, U, D, L, R, E dpending on hit button
void newReferenceWeight(void); // entering new values for reference weight
void calibScale(int loc); //AutoCalib Scale with new reference weight
int readBattVoltage(boolean *bWarn); //check batt value
void toOLED(char* row1, char* row2, char* row3, char* row4, char* row5); //output to OLED using u8g lib

void setup()
{
	int value_new = 0;
	byte ScaleFront_rdy = 0;
	byte ScaleBack_rdy = 0;
	int stabilisingtime = 3000;

	Serial.begin(9600); //open serial for debugging
	pinMode(buttonPin, INPUT); //Input Pin for buttons
	pinMode(batRefPin, INPUT); //Input Pin for Battery check
	u8g.setFont(u8g_font_unifont);
	
	toOLED("", "", " Scale SG 1.0", "", "");
	delay(2000);
	u8g.setFont(u8g_font_helvR08);

	EEPROM.get(adrFirstRun, firstRun);//check if we calibrated at least once
	if (firstRun == 1) {  //read only if EEPROM was written at least once
		EEPROM.get(adrTotalWeight, gewicht);
		EEPROM.get(adrSF_front, SF_front);
		EEPROM.get(adrSF_back, SF_back);
		//Serial.println((String)"Gew: " + gewicht + " SF_front: " + SF_front + " SF_Back: " + SF_back);
		toOLED("", "Starting Scale", "", "       ....please wait", "");
	}
	else {
		//Serial.println("StartElse");
		toOLED("Please calibrate", "at least once!", "", "Set Ref weight", "");
	}//end if firstRun

	delay(1000);

	ScaleFront.begin(); //satrt the Scales
	ScaleBack.begin();
	ScaleFront.start(2000); //Stabilize Sacales
	ScaleBack.start(2000);
	//Serial.println("Scales gestartet");

	while ((ScaleFront_rdy + ScaleBack_rdy) < 2) { //run startup, stabilisation and tare, both modules simultaneously
		if (!ScaleFront_rdy) ScaleFront_rdy = ScaleFront.startMultiple(stabilisingtime);
		if (!ScaleBack_rdy) ScaleBack_rdy = ScaleBack.startMultiple(stabilisingtime);
	}

	toOLED("", "", "Everything is ready now", "", "");
	//Serial.println("nach Tare");
	delay(1000);

	toOLED("", "      Setup new Reference", "              weight?", "", "  <    yes                no    >");
	//Serial.println("Warte auf Ja Nein");

	while (value_new != 99) {
		value_new = analogRead(buttonPin); // check left or right button
		delay(200);

		if (buttonhit(value_new) == 'R') {
			value_new = 99; //right button->exit
		};

		if (buttonhit(value_new) == 'L') { //left button go on entering new values for reference weight
			newReferenceWeight(); //enter the value for the new reference weight for calibrating scale
			calibScale(0); // calibrate frontScale
			calibScale(1); // calibrate backScale
			firstRun = 1;
			EEPROM.put(adrFirstRun, firstRun);
			value_new = 99; //exit nach Eingabe des Reference Gewichts und Kalibrierrung
		}
	};//end while



	ScaleFront.setCalFactor(SF_front); // set calibration factor
	ScaleBack.setCalFactor(SF_back); // set calibration factor

	ScaleFront.tareNoDelay();  //Calibweight has to be removed beforehand
	ScaleBack.tareNoDelay();

	toOLED("Startup + tare is complete", "", "       Put Plane on Scale", "", "");
	delay(2000);

	

}//end setup

void loop()
{
	
	float CGratio;
	long int CG;
	long int weightTot;
	const int printInterval = 500; // LCD/Serial refresh interval
	char weightLCD[17] = "displ";
	char CGtoLCD[17] = "displ";
    char batToLCD[17] = "";
	int t1;
	u8g.setFont(u8g_font_unifont);
	ScaleFront.update();
	ScaleBack.update();
	

	if (t1 < millis()) {  //start calculation
		t1 = millis() + printInterval;
		SW_front = ScaleFront.getData();
		SW_back = ScaleBack.getData();
		
		weightTot = (SW_back + SW_front) * 100;

		if (SW_front > 5 && SW_back > 5) {
			SW_front = SW_front / 10;
			SW_back = SW_back / 10;
			CGratio = (((SW_back * 10000) / (SW_front + SW_back)));
			CG = ((((DistSupPins)* CGratio) / 1000) - ((DistSupPins * 10) / 2) + CGoffset);
		}
		else
			CG = 0;


		if (weightTot < 0 && weightTot >= -100)
			weightTot = 0;

		if (weightTot < -100)
			sprintf(weightLCD, "Error");
		else
			sprintf (weightLCD,"Weight: %ld g",weightTot/100);

		if (CG != 0)
			sprintf(CGtoLCD, "CG: %ld mm", CG/100);
		else
			sprintf(CGtoLCD, "noCG");
			

		// Battery value display                     
		int batVal = readBattVoltage(&bBatWarning);
		if (bBatWarning)
		    sprintf(batToLCD,"Bat low: %d.%d%d V", (batVal / 1000), ((batVal % 1000) / 100), ((batVal % 100) / 10));
		else
		    sprintf(batToLCD, "Bat ok: %d.%d%d V", (batVal / 1000), ((batVal % 1000) / 100), ((batVal % 100) / 10));

		
		//BatVal, Weight and Cg OLED display      
		//Serial.println((String)"Bat: " + bat + " Weight: " + weightTot + " CG: " + CG);
		toOLED(batToLCD,"", weightLCD, "", CGtoLCD); 

	} //end if...t1 millis
} //end loop


void calibScale(int loc) {
	char checkEnter = 'n'; // just for initializing
	char wo[6] = "no";
	char help[12] = "empty";
	int value_new = 0;
	int i = 0;
		
	//Serial.println((String)"Calib "+loc);
	if (loc == 0)
		sprintf(wo,"FRONT");
	else
		sprintf(wo,"BACK");

	sprintf(help,"        %s Scale",wo);
	toOLED("Put weight to","", help,"", "If done...     press E");

	while (checkEnter != 'E') {
		value_new = analogRead(buttonPin); // check for Enter
		checkEnter = buttonhit(value_new);
		delay(100);
	}//end while

	if (loc == 0) {  //front Scale
		ScaleFront.setCalFactor(SF_front); //set default to be faster
		ScaleFront.update();
		delay(200);
		SW_front = ScaleFront.getData();
		SF_front = ScaleFront.getCalFactor();
			
		//Serial.print("Messgew: "); Serial.println(gewicht); Serial.print("Gew1 vo: "); Serial.println(SW_front); Serial.print("Fak1: "); Serial.println(SF_front);
			
		
		while (abs(SW_front - gewicht) >= 0.6) {
			ScaleFront.update();
			delay(200);
			if (i == 0){ //just to see on display that programm is not hanging
				toOLED("FRONT Scale", "", "    Calibration is running....", "", "");
				i = 1;
			}
			else {
				toOLED("FRONT Scale", "", "      Calibration is running", "", "");
					i = 0;
			}// ende else (i==0)


			//Serial.print("Gew vo: "); Serial.println(SW_front); Serial.print("Fak: "); Serial.println(SF_front);
			
			if (SW_front < gewicht) { //start autocalib
				SF_front = SF_front - 1.0;
				ScaleFront.setCalFactor(SF_front);
			}

			if (SW_front > gewicht) {
				SF_front = SF_front + 1.0;
				ScaleFront.setCalFactor(SF_front);
			}
			SW_front = ScaleFront.getData();
			
		}// end while 
		EEPROM.put(adrSF_front, SF_front);  //write calibrated value to EEPROM
	} //end if loc='0' (front calibration)

	if (loc == 1) {  //Back Scale
		ScaleBack.setCalFactor(SF_back); //set default to be faster
		ScaleBack.update();
		delay(200);
			SW_back = ScaleBack.getData();
			SF_back = ScaleBack.getCalFactor();
			
			//Serial.print("Messgew: "); Serial.println(gewicht); Serial.print("Gew1 hi: "); Serial.println(SW_back); Serial.print("Fak1: "); Serial.println(SF_back);
		while (abs(SW_back - gewicht) >= 0.6) {
			ScaleBack.update();
			delay(200);
			if (i == 0) { //just to see on display that programm is not hanging
				toOLED("BACK Scale", "", "   Calibration is running....", "", "");
				i = 1;
			}
			else {
				toOLED("BACK Scale", "", "     Calibration is running", "", "");
				i = 0;
			}// ende else (i==0)
			
			//Serial.print("Gew hi: "); Serial.println(SW_back); Serial.print("Fak: "); Serial.print(SF_back); Serial.print(" Diff:"); Serial.println(abs(SW_back - gewicht));
			
			if (SW_back < gewicht) { //start autocalib
				SF_back = SF_back - 1.0;
				ScaleBack.setCalFactor(SF_back);
				
			}

			if (SW_back > gewicht) {
				SF_back = SF_back + 1.0;
				ScaleBack.setCalFactor(SF_back);
				
			}
			
			SW_back = ScaleBack.getData();
				
		}// end while 
		EEPROM.put(adrSF_back, SF_back);  //write calibrated value to EEPROM
	} //end if loc='1' (back calibration)


	toOLED("Remove weight from", help, "", "", "If done...     press E");
	value_new = 0;
	checkEnter = 'n';
	while (checkEnter != 'E') {
		value_new = analogRead(buttonPin); // check for Enter
		checkEnter = buttonhit(value_new);
		delay(100);
	}//end while
	EEPROM.put(adrFirstRun, 1); //we are sure that values are written at least once to EEPROM
}// end CalibScale


char buttonhit(int value) {
	if (value >= 785 && value <= 790) {
		return ('L');
	}
	if (value >= 850 && value <= 854) {
		return ('R');
	}
	if (value >= 728 && value <= 732) {
		return ('U');
	}
	if (value >= 1020 && value <= 1025) {
		return ('D');
	}
	if (value >= 928 && value <= 932) {
		return ('E');
	}
}//end buttonhit


void newReferenceWeight(void) {

	int eingabewert = 0;
	int counter = 0;
	int value_new = 0;
	int tausend = 0;
	int hundert = 0;
	int zehn = 0;
	int eins = 0;
	char mass[8] = "0";
	char dist[6] = "";
	char cursor[11] = "-  -  -  -";


	//Serial.println("Subroutine RefGewicht");
	toOLED("Enter new value (gramm)", "",mass, cursor, "");
	sprintf(cursor, "-");
	while (value_new != 101) {
		value_new = analogRead(buttonPin);
		delay(200);

		switch (buttonhit(value_new)) {
		case 'U': //button up
			eingabewert = eingabewert + 1;
			if (eingabewert == 10) //after 9 back to 0
				eingabewert = 0;

			sprintf(mass, "%s%d", dist, eingabewert);
			
			toOLED("Enter new value","", mass, cursor, "");
			break;
		case 'D': //button down
			eingabewert = eingabewert - 1;
			if (eingabewert == -1) //after 0 back to 9
				eingabewert = 9;
			sprintf(mass, "%s%d", dist, eingabewert);
			toOLED("Enter new value","", mass, cursor, "");
			break;
		}// switch buttonhit


		if (buttonhit(value_new) == 'E') {//Bestätigung mit Enter
			switch (counter) {
			case 0:
				tausend = eingabewert;
				sprintf(dist, "%d ", tausend);
				sprintf(cursor, "-  -");
				sprintf(mass, "%s%d", dist, 0);
				toOLED("Enter new value", "",mass, cursor, "");
				break;
			case 1:
				hundert = eingabewert;
				sprintf(dist, "%d %d ", tausend, hundert);
				sprintf(cursor, "-  -  -");
				sprintf(mass, "%s%d", dist, 0);
				toOLED("Enter new value","", mass, cursor, "");
				break;
			case 2:
				zehn = eingabewert;
				sprintf(dist, "%d %d %d ", tausend, hundert, zehn);
				sprintf(cursor, "-  -  -  -");
				sprintf(mass, "%s%d", dist, 0);
				toOLED("Enter new value", "", mass, cursor, "");
				break;
			case 3:
				eins = eingabewert;
				sprintf(dist, "%d %d %d %d", tausend, hundert, zehn, eins);
				value_new = 101; //exit
				break;
			}//end switch 

			counter = counter + 1;
			eingabewert = 0;

			delay(100);//avoid double enter


		}; //end if buttonhit
	};//end while

	gewicht = tausend * 1000 + hundert * 100 + zehn * 10 + eins;
	sprintf(cursor, "     %d g", gewicht);
	toOLED("New Ref.Weight", "",cursor, "", "");

	delay(3000);
};// end newReferencWeight/ end newReferencWeight

int readBattVoltage(boolean *bWarn) { // read battery voltage
	long battvalue = 0;
	/*--- Double read to increase the stability ---*/
	battvalue += analogRead(batRefPin);
	battvalue += analogRead(batRefPin);
	/*--- Simple cross product and bride resistor divider value * 2 because we have done two analog read ---*/
	battvalue *= 4883L; // analog reading * (5.00V*1000000)/1024 (adjust value if VCC is not 5.0V)
	battvalue /= 640L; // this number comes from the resistor divider value ((R2/(R1+R2))*1000)/noof analogreadings (adjust value if required)
	/*--- Warning if battvalue < 5,5 V. Old Value 7,5 V ----*/
	if (battvalue < 5500) {
		*bWarn = TRUE;
	}

	return battvalue;
} /* end readBattVoltage() */


void toOLED(char* row1, char* row2, char* row3, char* row4, char* row5) {
	
	u8g.firstPage();
	do {
		u8g.setPrintPos(0, line1);
		u8g.print(row1);
		u8g.setPrintPos(0, line2);
		u8g.print(row2);
		u8g.setPrintPos(0, line3);
		u8g.print(row3);
		u8g.setPrintPos(0, line4);
		u8g.print(row4);
		u8g.setPrintPos(0, line5);
		u8g.print(row5);
	} while (u8g.nextPage());
}//end void toOLED
