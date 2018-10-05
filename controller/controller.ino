/*
 * so far this is a simple remote controll for onkyo receivers
 * planned to add cool features
 * connect 
 *   - a button to pin12 and ground
 *   - a (yellow or blue) led to pin13 and ground
 * create local file credentials.h with wifi ssid and pw:
 *   #define WIFI_SSID "your_ssid"
 *   #define WIFI_PASSWD "your_pw"
*/
#include <Arduino.h>
#include <WiFi.h>
#include <JC_Button.h>
#include <MFRC522.h>
#include "credentials.h"

const char* host = "192.168.178.37"; // ip address of onkyo receiver
const uint16_t port = 60128;
WiFiClient client;

int ledPin=13;
int buttonPin=12;
Button myButton(buttonPin);
bool ignoreBtn=true;
const unsigned long LONG_PRESS(1000);           // we define a "long press" to be 1000 milliseconds.
char* options[16]; 
int layer=0;
int nOpt=0;
char serial_cmd[256]; //buffer for serial commands

//state of receiver
int pow_main=0;
int vol_main=0;
int status_playing=0; //stop/pause/play/ff/fr
char* input;
int serial_cmd_pt=0;

// MFRC522 RFID card interface
#define RST_PIN 22                 // Configurable
#define SS_PIN 21                // Configurable
MFRC522 mfrc522(SS_PIN, RST_PIN); // Create MFRC522
MFRC522::MIFARE_Key key;
bool successRead;
byte sector = 1;
byte blockAddr = 4;
byte trailerBlock = 7;
MFRC522::StatusCode status;

bool handleResponse(int =100, char*  ="PWR00");

void setup()
{
    //pinMode(buttonPin, INPUT_PULLUP);
    //pinMode(buttonPin, INPUT);
    myButton.begin();
    pinMode(ledPin, OUTPUT);
    
    digitalWrite(ledPin, LOW);
    
    
    Serial.begin(115200);
    
    delay(10);
    
    // initialize MFRC522 Card Reader
    SPI.begin();        // Init SPI bus
    mfrc522.PCD_Init(); // Init MFRC522
    mfrc522.PCD_DumpVersionToSerial(); // Print details of device
    for (byte i = 0; i < 6; i++) {
      key.keyByte[i] = 0xFF;
    }
    
    Serial.println();
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(WIFI_SSID);
   
    WiFi.begin(WIFI_SSID, WIFI_PASSWD);
    WiFi.setHostname("home_controller");
    while (WiFi.status() != WL_CONNECTED) {
        delay(100);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void loop()
{    
    
    if (!client.connected()) {
      Serial.print("connecting to ");
      Serial.println(host);
      if (!client.connect(host, port)) {
        Serial.println("connection failed");
        delay(1000);
        return;
      }
      // send status requests to the server
      delay(50);
      sendCommand("PWRQSTN");//Power status
      sendCommand("MVLQSTN");//master volume
      sendCommand("NSTQSTN");//network play status
      
      //sendCommand("ECNQSTN");//??
      //sendCommand("SLIQSTN");//input select
      //sendCommand("SLZQSTN");//zone2 input select
    }
    
    readSerial(); //check for commands from serial           
            
    // Read all the lines of the reply from server and print them to Serial    
    handleResponse();
    //handle buttons
    myButton.read();
    if (myButton.wasReleased() && ! ignoreBtn){
      togglePlay();
    }else if (myButton.pressedFor(LONG_PRESS) && ! ignoreBtn){
      togglePower();
      ignoreBtn=true; //gets reactivated if receiver has send response
    }
    if(mfrc522.PICC_IsNewCardPresent()){
 
      // Show some details of the PICC (that is: the tag/card)
      Serial.print(F("Card UID:"));
      printAsHex(mfrc522.uid.uidByte, mfrc522.uid.size);
      Serial.println();
      playUSB("hochzeit");
    }
    
    delay(100);//wait 0.1 seconds
}

void readSerial(){
  while(Serial.available()) {
     serial_cmd[serial_cmd_pt]=Serial.read();
     if(serial_cmd[serial_cmd_pt] == 0x0D){//ignore rest of the input after newline
       serial_cmd[serial_cmd_pt] = 0x00;
       while(Serial.available()){
        Serial.read();
       }
     }
     if(serial_cmd[serial_cmd_pt] == 0x00){
       if(strcmp(serial_cmd, "playUSB")==0){
         playUSB("hochzeit");
       }else{
         sendCommand(serial_cmd);
       }
       serial_cmd_pt=-1;
     }
     serial_cmd_pt+=1;
  }
}
void playUSB(char* folder){
  
  Serial.println("playUSB...");

  unsigned long timeout=2000;//wait max 2 second for response
  if (pow_main==0){
    sendCommand("PWR01");
    if (!waitForResponse(timeout, "PWR01")) return;
    delay(1000); //wait a second to boot up usb
  }
  //if(strcmp(input, "USB Rear")!=0){
  sendCommand("SLI2A");
  if (!waitForResponse(timeout, "SLI2A")) return;
  //}
  
  sendCommand("NTCTOP"); // go to top level
  if (!waitForResponse(timeout, "#NWOPT")) return;
  
  sendCommand("NLSL0"); //USB Storage (may be no item --> check!)
  if (!waitForResponse(timeout, "#NWOPT")) return;
 
  sendCommand("NLSL0"); //first folder (mp3)
  if (!waitForResponse(timeout, "#NWOPT")) return;
  
  sendCommand("NLSL0"); //first subfolder (hochzeit)
  if (!waitForResponse(timeout, "#NWOPT")) return;
  
  sendCommand("NLSL0"); //Play first file 
  if (!waitForResponse(timeout, "#NWOPT")) return;
  
  
}

bool waitForResponse(int timeout, char* cmd){
  unsigned long timestamp = millis();
  while(!handleResponse(100,cmd)){
      if(millis() - timestamp > timeout){
        Serial.println("timeout");
        return false;
      }
  }
  return true;
}
void togglePower(){
  if (pow_main){
    sendCommand("PWR00");
    digitalWrite(ledPin, LOW);//to see imediate action
  }else{
    sendCommand("PWR01");
    digitalWrite(ledPin, HIGH);
  }

}
void togglePlay(){
  //sendCommand("NTCP/P"); does not work
    
   if (status_playing){
    sendCommand("NTCPAUSE");
     //status_playing=1;
  }else{ //stop or pause
    sendCommand("NTCPLAY");
    //status_playing=2;
  }
 
}

bool handleResponse(int timeout, char* check){
    int read_lines=0;
    int prev_read;
    char header[16];
    char payload[256];
    bool found=false;
    do{
      prev_read=read_lines;
      unsigned long timestamp = millis();
      while (client.available() == 0) {
        if (millis() - timestamp > timeout) {
          //Serial.print("---read ");
          //Serial.print(read_lines);
          //Serial.println(" messages---");
          return found;
        }
        delay(10);
      }
      while(client.available()) {
        //String line = client.readStringUntil(0x0A);        
        //String response=getResponse();
        readBuffer(header,16);
        
        if (strncmp(header, "ISCP",4)==0){
          int msg_size=(int)header[11]+(int)header[10]*256+(int)header[9]*pow(256,2)+(int)header[8]*pow(256,3);
          //Serial.print("message length: ");
          //Serial.println(msg_size);
          readBuffer(payload,msg_size);
          Serial.println(payload+2);
          parseCmd(payload+2);          
          if (checkCmd(payload+2, check)){ 
            Serial.println("found what we where looking for");
            found=true;
          }
          read_lines++;        
          
        }else{
          Serial.print("Invalid response header: ");
          Serial.println(header);
        }
      }
      Serial.flush();
    }while(prev_read<read_lines);
    return found;
}
bool checkCmd(char* cmd1, char* cmd2){
  int n=min(strlen(cmd1)-2, strlen(cmd2));
  if(strncmp(cmd1, cmd2,n )==0){
    return true;
  }else if(strncmp(cmd2, "#NWOPT",n )==0 && strncmp(cmd1, "NLS",3 )==0){ //todo: check if all options are there
    return true;
  }
  return false;
}
void readBuffer(char* buf, int n){
  for(int i=0;i<n;++i){
    buf[i]=client.read();
    //Serial.write(String(buf[i],16).c_str());
    //Serial.write(' ');
  }
  buf[n-1]=0x00;
  //Serial.println("---");
  //Serial.print(buf);
  
}
void sendCommand(String cmd){
    Serial.println("sending command: "+cmd);
    int msz=16+cmd.length()+3;
    byte message[msz]={ 'I','S','C','P',
                       0,0,0,16,//header size
                       0,0,0,cmd.length()+5,//message size
                       1,0,0,0,//version + 3 reserved bit
                       '!','1'};//begin of command
    cmd.toCharArray(((char *)message)+18,cmd.length()+1);
    message[msz-1]=0x0D;
    //message[msz-2]=0x0A;
    //Serial.write(message,sizeof(message) );
    client.write(message, sizeof(message));
    client.flush();
}
void printAsHex(byte *buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? ":0" : ":");
    Serial.print(buffer[i], HEX);
    if(i%4==0){
      Serial.print(" ");
    }
  }  
}
void parseCmd(char* cmd){  
  char * pEnd;
  long int val= strtol (cmd+3,&pEnd,16);

  if(strncmp(cmd, "PWR",3)==0){
    Serial.print("Power is now ");
    Serial.println(val);
    pow_main=val;
    ignoreBtn=false;
    if (pow_main) {
      digitalWrite(ledPin, HIGH); 
    } else {
      digitalWrite(ledPin, LOW);
    }
  }else if(strncmp(cmd, "NST",3)==0){
    /*NET/USB Play Status (3 letters)
      p -> Play Status: "S": STOP, "P": Play, "p": Pause, "F": FF, "R": FR, "E": EOF
      r -> Repeat Status: "-": Off, "R": All, "F": Folder, "1": Repeat 1, "x": disable
      s -> Shuffle Status: "-": Off, "S": All , "A": Album, "F": Folder, "x": disable
    */
    
    Serial.print("Network state: ");
    Serial.println(pEnd);    
    switch(pEnd[0]){
      case 'S':
        status_playing=0;
        break;
      case 'p':
        status_playing=1;
        break;
      case 'P':
        status_playing=2;
        break;
      case 'F':
        status_playing=3;
        break;
      case 'R':
        status_playing=4;
        break;    
    }
  }else if(strncmp(cmd, "NLT",3)==0){ //Network List title
    /*[0,1] service type
      [3]   UI type
      [4]   layer
      [5-8] current position
      [9-12]number of items
      [13,14] number of layers
      [15,16] reserved
      [17-20] icon left and right
      [21-22] status (network)
      [23..] Title Name
    */
    switch(pEnd[0]){//not all defined options are considered
      case '0':
        switch(pEnd[1]){
          case '0':
            input="DLNA";
            break;
          case '1':
            input="Favorite";
            break;
          case 'A':
            input="Spotify";
            break;
          default:
            input="unknown";
        }
        break;
      case 'F':
        switch(pEnd[1]){
          case '0':
            input="USB Front";
            break;
          case '1':
            input="USB Rear";
            break;
          case '2':
            input="Internet Radio";
            break;
          case '3':
            input="Net";
            break;
          default:
            input="unknown";
        }
        break;
      default:
        input="unknown";
    }//end pEnd[0]
    //number of options
    
  }//END NLT
}

