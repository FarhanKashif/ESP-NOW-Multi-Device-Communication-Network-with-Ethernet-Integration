#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <map>
#include <ETH.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <stdint.h>

#define MAX_TRIES 3
#define EEPROM_SIZE 512
#define MAX_NODES 9
#define MAC_SIZE 6

/* NOTE: In order for your Node to communicate with any other node. The two must be connected to eath other with encryption enabled.
Refer to this https://github.com/FarhanKashif/Scalable-ESP32-Mesh-Network-Using-ESP-NOW for more information. */

const uint8_t node3[] = {0x24, 0xDC, 0xC3, 0xC6, 0xAE, 0xCC}; // Replace with MAC of Node from where you want to receive data 

/* Server URL */
const char serverURL[] = "http://192.168.1.1:5000/receive_data";  // replace with your local host (Ensure Port 5000 is not being used)

// PMK & LMK Keys
static const char *PMK_KEY = "Connection_ESP32"; // 16-byte PMK
static const char *LMK_KEY = "LMK@ESP32_123456"; // 16-byte LMK

/* Project Variables */
uint8_t baseMac[6]; // Base MAC Address of Sender
unsigned long startTime, endTime; // Timer Variables
int incoming_data_count = 0; // Incoming Data Count
std::map<int, bool> receivedpackets;  // Track of Packet ID's
int counter = 1;  // Session Counter

/* PACKET STRUCTURE */
typedef struct message {
  unsigned char text[64]; // 64 bytes of text
  //int value; 
  //float temperature;
  int TTL; // Time to live for packet 
  int identification; // 1-> BROADCAST, 2-> DATA
  bool broadcast_Ack; //  0 -> Default Communication, 1 -> Acknowledgement of Broadcast
  bool Data_Ack; // 0 -> Default Communication, 1 -> Acknowledgement of Data
  uint8_t destination_mac[6]; // MAC Address of Receiver
  uint8_t source_mac[6]; // MAC Address of Sender
  int packetID; // Packet ID
  uint8_t Path_Array[MAX_NODES][MAC_SIZE]; // Path Array
  uint8_t Path_Index; // Index of Path Array
  uint8_t Path_Length;  // Length of Path Array
  bool Path_Exist;  // Check if Path Exists
} message_t;

message_t msg, copy_msg;

enum State {
  WAITING_FOR_ACK,
  READY_TO_SEND,
};

State currentState = READY_TO_SEND; // Initial State of Device

// Queue Structure
typedef struct queue_node {
  message_t data;
  uint8_t mac[6];
  struct queue_node *next;
} queue_node_t;

/* FUNCTION DEFINITIONS */
void Add_Peer(const uint8_t* mac);
void SwitchToEncryption(const uint8_t *mac);
void On_Data_Receive(const uint8_t* mac, const uint8_t* data, int len);
void readMAC();
void On_Data_Sent(const uint8_t *mac_addr, esp_now_send_status_t status);
void ProcessReceivedData();
bool AppendBaseMAC(uint8_t index);
void ReverseArray(uint8_t index, uint8_t path_arr[MAX_NODES][MAC_SIZE]);
void FollowPathArray(queue_node_t *temp);
void PrintArray(uint8_t path_arr[MAX_NODES][MAC_SIZE], uint8_t index);
bool Configure_Packet(const char *text, int TTL, int identification, bool broadcast_Ack, bool Data_Ack, const uint8_t *destination_mac, const uint8_t *source_mac, bool path_exist);
void Check_Existing_Peer(const uint8_t* mac);
void SerializeData(queue_node_t *temp);
void SendDataToServer(String jsonString);
void InitializeEthernet();
void TestEthernetConnection();

/* Queue Variables */
queue_node_t *front = NULL;
queue_node_t *rear = NULL;

// Append Base MAC to Path Array
bool AppendBaseMAC(uint8_t index) {
  
  Serial.printf("Inside Append MAC\n");

  if(index >= MAX_NODES) {
    Serial.println("Max Nodes Reached. Cannot Append MAC Address.");
    return false;
  }

  // Copy Base MAC Address to Path Array
  memcpy(msg.Path_Array[index], baseMac, 6);
  Serial.printf("Appended MAC at index: %d\n", index);
  msg.Path_Index = ++index; // Increment Index of Path Array
  ++msg.Path_Length; // Increment Length of Path Array
  return true;
}

// Send Data to Flask Server
void SendDataToServer(String jsonString) {

  HTTPClient http;
  http.begin(serverURL);
  http.addHeader("Content-Type", "application/json"); // Set the content type to JSON

  // Send the JSON string via POST request
  int httpResponseCode = http.POST(jsonString);

  // Check the response code
  if (httpResponseCode > 0) {
    String response = http.getString(); // Get the response from the server
    Serial.print("POST Response code: "); // Print the response code
    Serial.println(httpResponseCode); // Print the HTTP response code
    Serial.println(response); // Print the server response
  } else {
    Serial.print("Error on sending POST: "); // Print error message
    Serial.println(httpResponseCode); // Print the error code
  }

  http.end(); // End Connection

}
// Serialize data
void SerializeData(queue_node_t *temp) {

  //StaticJsonDocument<200> jsonDoc; 
  DynamicJsonDocument jsonDoc(200);

  jsonDoc["text"] = (const char*)temp->data.text;
  jsonDoc["TTL"] = temp->data.TTL;
  jsonDoc["identification"] = temp->data.identification;
  jsonDoc["broadcast_Ack"] = temp->data.broadcast_Ack;
  jsonDoc["Data_Ack"] = temp->data.Data_Ack;
  jsonDoc["packetID"] = temp->data.packetID;
  jsonDoc["Path_Index"] = temp->data.Path_Index;
  jsonDoc["Path_Length"] = temp->data.Path_Length;
  jsonDoc["Path_Exist"] = temp->data.Path_Exist;

  // Serialize the Path_Array
  JsonArray pathArray = jsonDoc["Path_Array"].to<JsonArray>();
  for (int i = 0; i <= temp->data.Path_Index; i++) {
    JsonArray pathElement = pathArray.add<JsonArray>();
    for (int j = 0; j < MAC_SIZE; j++) {
      pathElement.add(temp->data.Path_Array[i][j]);
    }
  }

  // Serialize Source MAC
  JsonArray sourceMAC = jsonDoc["SourceMAC"].to<JsonArray>();
  for(int i=0;i<MAC_SIZE;i++) {
    sourceMAC.add(temp->data.source_mac[i]);
  }
 
  String jsonString;
  serializeJson(jsonDoc, jsonString);

  // Send Data to Flask Server
  SendDataToServer(jsonString);

}
// Callback when data is sent
void On_Data_Sent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Packet Successfully Sent to: ");
  startTime = millis(); // Start Timer
  for(int i=0;i<5;i++) {
    Serial.print(mac_addr[i], HEX);
    if(i < 5) {
      Serial.print(":");
    }
    if(i==5) {
      Serial.println(""); // Print Next Line
    }
  }

}

void PrintArray(uint8_t path_arr[MAX_NODES][MAC_SIZE], uint8_t index) {
  for(int i=0;i<=index;i++) {
    Serial.printf("MAC at index %d: %02X:%02X:%02X:%02X:%02X:%02X\n",i, path_arr[i][0], path_arr[i][1], path_arr[i][2], path_arr[i][3], path_arr[i][4], path_arr[i][5]);
  }
}

// Reverse Array
void ReverseArray(uint8_t index, uint8_t path_arr[MAX_NODES][MAC_SIZE]) {
  uint8_t temp[MAC_SIZE];
  uint8_t size = index;
  ++size;
  for (int i = 0; i < size/2; ++i) {
      memcpy(temp, path_arr[i], MAC_SIZE);  // Copy current to temp
      memcpy(path_arr[i], path_arr[size - 1 - i], MAC_SIZE);  // Copy last element to current index
      memcpy(path_arr[size - 1 - i], temp, MAC_SIZE);  // Copy current element to last index
  }

  Serial.println("Row reversed Successfully.");
}


// Read MAC Address of ESP32
void readMAC()
{
  if(esp_read_mac(baseMac, ESP_MAC_WIFI_STA) == ESP_OK)
  {
    Serial.printf("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  } else {
    Serial.println("Failed to read MAC address..");
  }
}


// Send Data to Next Hop in Path
void FollowPathArray(queue_node_t *temp) {

    // Copy Path Array to Packet Path Array
    Serial.println("Inside Follow Path Array.");
    PrintArray(msg.Path_Array, MAX_NODES);

    ++temp->data.Path_Index; // Increment Path Index (1)
    msg.Path_Index = temp->data.Path_Index; // Copy Index to Packet Path Index (copies 1 to msg.Path_Index)

    Serial.printf("Sending to MAC: %02X:%02X:%02X:%02X:%02X:%02X at index %d\n", temp->data.Path_Array[temp->data.Path_Index][0], temp->data.Path_Array[temp->data.Path_Index][1], temp->data.Path_Array[temp->data.Path_Index][2], temp->data.Path_Array[temp->data.Path_Index][3], temp->data.Path_Array[temp->data.Path_Index][4], temp->data.Path_Array[temp->data.Path_Index][5], temp->data.Path_Index);

    // Send Packet to Next MAC in Path Array
    esp_err_t result = esp_now_send(temp->data.Path_Array[temp->data.Path_Index], (uint8_t *) &msg, sizeof(msg));
    if(result == ESP_OK) {
      Serial.printf("Acknowledgement Sent with Success to MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", temp->data.Path_Array[temp->data.Path_Index][0], temp->data.Path_Array[temp->data.Path_Index][1], temp->data.Path_Array[temp->data.Path_Index][2], temp->data.Path_Array[temp->data.Path_Index][3], temp->data.Path_Array[temp->data.Path_Index][4], temp->data.Path_Array[temp->data.Path_Index][5]);
    } else {
      Serial.println("Error while sending Data to Path.");
      Serial.println(esp_err_to_name(result));
    }

}

// Enable Encryption for MAC Address
void SwitchToEncryption(const uint8_t *mac) {
  if(esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peerInfo = {};

    if(esp_now_get_peer(mac, &peerInfo) == ESP_OK) {
      if(peerInfo.encrypt) {
        Serial.println("Encryption Mode Already Enabled.");
        Serial.printf("Peer MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", peerInfo.peer_addr[0], peerInfo.peer_addr[1], peerInfo.peer_addr[2], peerInfo.peer_addr[3], peerInfo.peer_addr[4], peerInfo.peer_addr[5]);
        return; // Return if Encryption Mode is already enabled
      } else {
        Serial.println("Encryption Mode Not Enabled. Enabling Encryption Mode.");
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, mac, 6);
        peerInfo.channel = 0;
        
        for(uint8_t i = 0; i<16; i++) {
          peerInfo.lmk[i] = LMK_KEY[i];
        }
        peerInfo.encrypt = true;
        if(esp_now_del_peer(mac) == ESP_OK) {
          Serial.println("Peer Deleted Successfully.");
        } else {
          Serial.println("Failed to Delete Peer.");
        }
        
        if(esp_now_add_peer(&peerInfo) == ESP_OK) {
          Serial.println("Encryption Mode Successfully Enabled.");
        } else {
          Serial.println("Failed to Add Peer With Encryption.");
        }
      }
    } else {
      Serial.println("Failed to Fetch Peer Information.");
    }
  }
}

// Add Peer in Routing Table
void Add_Peer(const uint8_t* mac) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  
  for(uint8_t i = 0; i<16; i++) {
    peerInfo.lmk[i] = LMK_KEY[i];
  }

  peerInfo.encrypt = false; // Enable Encryption Mode

  esp_err_t status = esp_now_add_peer(&peerInfo);

  // Print MAC Address
  if (status == ESP_OK) {
    Serial.println("Peer Added Successfully");
    Serial.print("Peer MAC: ");
    for (int i = 0; i < 6; i++) {
      Serial.print(mac[i], HEX);
      if (i < 5) {
        Serial.print(":");
      } 
      if (i == 5) {
        Serial.println(""); // print a new line
      }
    }
  } else {
    Serial.println("Error Adding Peer.");
    Serial.println(esp_err_to_name(status));
  }
}

// Check if Peer Already Exists
void Check_Existing_Peer(const uint8_t* mac)
{
  bool exists = esp_now_is_peer_exist(mac);
  if(!exists) {
    Serial.println("New Peer Found.");
    Serial.println("Adding Peer");
    Add_Peer(mac);  // Add New Peer to network
    SwitchToEncryption(mac);  // Switch to Encryption Mode
  }
}

// Configure Packet
bool Configure_Packet(const char *text, int TTL, int identification, bool broadcast_Ack, bool Data_Ack, const uint8_t *destination_mac, const uint8_t *source_mac, bool path_exist) {
  
  /*if(mac == NULL || data == NULL || destination_mac == NULL || source_mac == NULL) {
    return false;
  }*/

  memset(&msg, 0, sizeof(msg)); // Clear Packet
  strncpy((char *)msg.text, text, sizeof(msg.text) - 1); // Copy Data to Message
  msg.text[sizeof(msg.text) - 1] = '\0'; // Null Terminate
  msg.TTL = TTL; // Set Time to Live
  msg.identification = identification; // Set Identification
  msg.broadcast_Ack = broadcast_Ack; // Set Acknowledgement
  msg.Data_Ack = Data_Ack; // Set Data Acknowledgement
  int random = esp_random(); // Generate Random Packet ID
  msg.packetID = random; // Set Packet ID
  memcpy(msg.destination_mac, destination_mac, 6); // Set Destination MAC Address
  memcpy(msg.source_mac, source_mac, 6); // Set Source MAC Address
  msg.Path_Exist = path_exist; // Set Path Exist Flag
  msg.Path_Length = 0; // Set Path Length

  if(!path_exist && !broadcast_Ack && (identification == 2) && (!Data_Ack)) {  
    bool result = AppendBaseMAC(0); // Append Base MAC Address to Path Array
    if(result) {
      Serial.println("Appended MAC Successfully.");
    } else {
      Serial.println("Failed to Append MAC.");
    }
  }

  memcpy(&copy_msg, &msg, sizeof(msg)); // Make a Buffer of Packet

  return true;
}

// Callback when data is received
void On_Data_Receive(const uint8_t* mac, const uint8_t* data, int len) {
  Serial.println("Inside On_Data_Receive Function");
  // Copy data to msg structure
  endTime = millis(); // Stop Timer
  memcpy(&msg, data, sizeof(msg)); 

  // Handle Incoming Data 
  queue_node_t *new_node = (queue_node_t *)malloc(sizeof(queue_node_t));
  if(new_node == NULL) {
    Serial.println("Memory Allocation Failed.");
    return;
  }
  
  // Copy message details to new node
  strncpy((char*)new_node->data.text, (char*)data, sizeof(new_node->data.text) - 1);
  new_node->data.text[sizeof(new_node->data.text) - 1] = '\0';
  memcpy(new_node->mac, mac, 6); 
  new_node->data.identification = msg.identification;
  new_node->data.broadcast_Ack = msg.broadcast_Ack; 
  new_node->data.Data_Ack = msg.Data_Ack;
  new_node->data.TTL = msg.TTL;  
  new_node->data.packetID = msg.packetID;
  memcpy(new_node->data.destination_mac, msg.destination_mac, 6); // Set Destination MAC Address
  memcpy(new_node->data.source_mac, msg.source_mac, 6); // Set Source MAC Address
  new_node->data.Path_Index = (uint8_t)msg.Path_Index;  // Set Path Index
  new_node->data.Path_Length = (uint8_t)msg.Path_Length;  // Set Path Length
  new_node->data.Path_Exist = msg.Path_Exist;  // Set Path Exist Flag
  // Store Path Array in new node
  for(int i=0;i<MAX_NODES;i++) {
    memcpy(new_node->data.Path_Array[i], msg.Path_Array[i], MAC_SIZE);
  }
  new_node->next = NULL;

  //Configure_Packet((char*)new_node->data.text, new_node->data.TTL, new_node->data.identification, new_node->data.broadcast_Ack, new_node->data.Data_Ack, new_node->data.destination_mac, new_node->data.source_mac, new_node->data.Path_Exist);

  if(front == NULL && rear == NULL) {
    front = rear = new_node;
  } else {
    rear->next = new_node;
    rear = new_node;
  }
  Serial.print("TTL: ");
  Serial.println(msg.TTL);
  Serial.printf("Destionation MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", msg.destination_mac[0], msg.destination_mac[1], msg.destination_mac[2], msg.destination_mac[3], msg.destination_mac[4], msg.destination_mac[5]);

  // Check if the message is for this node
  if(memcmp(baseMac, msg.destination_mac, 6) == 0) {
    ++incoming_data_count; // Increment Incoming Data Count
  } 
  // Handle Broadcast Messages
  else if(memcmp(msg.destination_mac, "\xFF\xFF\xFF\xFF\xFF\xFF", 6) == 0) {
    Serial.println("Broadcast Message Received.");
    ++incoming_data_count; // Increment Incoming Data Count
  } else {
    Serial.println("TTL Expired. Discarding Packet.");
    memset(&msg, 0, sizeof(msg)); // Clear Message
    free(new_node); // Free Memory
  }
}

void ProcessReceivedData() {
  Serial.print("Packet ID: ");
  Serial.println(msg.packetID);
  // Check if Packet is already received
  if(receivedpackets[msg.packetID]) {
    Serial.println("Packet Already Received. Discarding Duplicate Packet.");
    return;
  }

  receivedpackets[msg.packetID] = true; // Mark Packet as Received

  if(front == NULL) {
    Serial.println("Queue is empty. No Data to Process");
    return;
  }

  queue_node_t *temp = front;

  Serial.println("Inside Processing Function");

  front = front->next;  // Point to next node in queue

  if(front == NULL) {
    rear = NULL;
  }

  switch(temp->data.identification) {
    case 2: // DATA is Received 
      Serial.println("*************************************************");
      Serial.println("");
      Serial.print("Session: ");
      Serial.println(counter);
      Serial.println("Session Started");
      Serial.print("Sender MAC Address: ");

      for (int i = 0; i < 6; i++) {
        Serial.print(temp->data.source_mac[i], HEX);
        if (i < 5) {
          Serial.print(":");
        } 
        if( i == 5) {
          Serial.println(""); // print a new line
        }
      }
      Serial.print("Packet ID: ");
      Serial.println(temp->data.packetID);
      if((bool *)temp->data.broadcast_Ack) {  // Process Broadcast Acknowledgement
        Serial.print("Broadcast Acknowldgement Received: ");
        Serial.println(temp->data.broadcast_Ack);
        Serial.println((char *) temp->data.text);
        Serial.println("*************************************************");
        memset(&copy_msg, 0, sizeof(copy_msg)); // Clear the Buffer After Successful Broadcast Acknowledgement
      } else if((bool *)temp->data.Data_Ack) {  // Process Data Acknowledgement
        Serial.print("Data Acknowledgement Received: ");
        Serial.println(temp->data.Data_Ack);
        Serial.println((char *) temp->data.text);
        Serial.println("Session Terminated");
        Serial.println("");
        Serial.println("*************************************************");
        memset(&copy_msg, 0, sizeof(copy_msg)); // Clear the Buffer After Successful Data Acknowledgement
        currentState = READY_TO_SEND; // Change State to READY_TO_SEND
      }
      else {
        Serial.print("Data Received: ");
        Serial.println((char*) temp->data.text);
        Serial.println("Session Terminated");
        Serial.println("");
        Serial.println("*************************************************");
        // Handle Sending Acknowledgement here for Data
        AppendBaseMAC(temp->data.Path_Index); // Append Dst Base MAC Address to Path Array
        PrintArray(msg.Path_Array, temp->data.Path_Index);  // Print Path Array
        memset(&temp->data.Path_Array,0,sizeof(temp->data.Path_Array)); // Clear Path Array
        for(int i=0;i<=temp->data.Path_Index;i++) {
          memcpy(temp->data.Path_Array[i], msg.Path_Array[i], MAC_SIZE);  // Updated with Dst MAC to Path Array
        }
        ReverseArray(temp->data.Path_Index, temp->data.Path_Array); // Reverse the Path Array
        PrintArray(temp->data.Path_Array, temp->data.Path_Index);  // Print Reversed Array
        //PrintArray(temp->data.Path_Array, temp->data.Path_Index);  // Print Reversed Array
        Check_Existing_Peer(temp->data.Path_Array[1]);  // Check if Next Hop Exists Else Add in Encryption Mode
        Configure_Packet("Ack from Node 3", 10, 2, false, true, temp->data.source_mac, baseMac, true); // Configure Packet
        msg.Path_Index = temp->data.Path_Index;  // Copy Path Index to Packet
        // Copy Path to Packet
        for(int i=0;i<=msg.Path_Index;i++) {
          Serial.printf("Copying data at index: %d\n", i);
          memcpy(msg.Path_Array[i], temp->data.Path_Array[i], MAC_SIZE);
        } 
        
        // Reset index to 0
        temp->data.Path_Index = 0;  // Reset Path Index
        //Serial.printf("Total Path Index (msg): %d", msg.Path_Index);
        //Serial.printf("Total Path Index (temp): %d", temp->data.Path_Index);
        // Send Data according to Path
        FollowPathArray(temp);
        // Serialize Data and Send to Server
        SerializeData(temp);
      }
    break;
    default:  // Unknown Message
      Serial.println("Unknown Message Received");
    break;
  }

  counter++;  // Increment session counter

  free(temp); // Free memory
}

void InitializeEthernet() {
  if (ETH.begin()) {
    Serial.println("Ethernet initialized");
    
    // Set static IP address for WT32-ETH01
    IPAddress localIP(192, 168, 1, 100); // Desired static IP of WT32
    IPAddress gateway(192, 168, 1, 1);   // Host Ethernet IP
    IPAddress subnet(255, 255, 255, 0);  // Subnet mask

    // Configure Ethernet with static IP
    if (!ETH.config(localIP, gateway, subnet)) {
      Serial.println("Failed to configure Ethernet with static IP");
    } else {
      Serial.println("Ethernet configured with static IP");
    }
  } else {
    Serial.println("Ethernet initialization failed");
  }
}

void TestEthernetConnection() {
    String jsonString = "{\"sensor_data\": {\"temperature\": 25.5, \"humidity\": 60.2}, \"timestamp\": 1624471200}";

    HTTPClient http;
    http.begin("http://192.168.1.1:5000/receive_data"); // Change to your endpoint
    http.addHeader("Content-Type", "application/json");
    int httpResponseCode = http.POST(jsonString); // Perform a GET request

    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.print("GET Response code: ");
      Serial.println(httpResponseCode);
      Serial.println(response);
    } else {
      Serial.print("Error on sending GET: ");
      Serial.println(httpResponseCode);
    }
    http.end();

}

void setup() {  
  Serial.begin(115200);

  WiFi.disconnect();
  WiFi.mode(WIFI_STA);  

  if(esp_wifi_init(NULL) != ESP_OK) {
    Serial.println("Failed to initialize WiFi");
  }

  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start(); 

  readMAC(); //Read MC MAC Addr

  esp_now_init(); // Initialize ESP-NOW

  esp_now_set_pmk((uint8_t *) PMK_KEY); // Set PMK Key

  Check_Existing_Peer(node3); // Check if Node 3 Exists

  esp_now_register_send_cb(On_Data_Sent); // Register send_cb function
  esp_now_register_recv_cb(On_Data_Receive); // Register receive_cb function

  InitializeEthernet(); // Assign Ethernet IP to WT32-ETH01

  delay(1000);
  srand(time(NULL));
}

float current_time, prev_time = 0;

void loop() {
  //current_time = millis();

  while(incoming_data_count > 0) {
    Serial.println(incoming_data_count);
    ProcessReceivedData();
    incoming_data_count--;
  }

  /*if(current_time - prev_time > 5000) {
    TestEthernetConnection();
    prev_time = current_time;
  }*/
}
