#include <Arduino.h>

// --- UART-COBS constants ---
#define UARTCL_VERSION_1 0x10
#define UARTCL_FLAG_DATA 0x00
#define UARTCL_FLAG_SYNC 0x08

uint32_t baseDtnTime = 0;
unsigned long syncMillis = 0;
bool timeSynced = false;
uint8_t seqNum = 0; // Bundle sequence number

// --- CRC-16 CCITT-FALSE ---
static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

uint16_t computeCrc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ crc16_table[(crc >> 8) ^ data[i]];
    }
    return crc;
}

size_t cobsEncode(const uint8_t *ptr, size_t length, uint8_t *dst) {
    size_t read_index = 0, write_index = 1, code_index = 0;
    uint8_t code = 1;
    while (read_index < length) {
        if (ptr[read_index] == 0) {
            dst[code_index] = code;
            code = 1;
            code_index = write_index++;
            read_index++;
        } else {
            dst[write_index++] = ptr[read_index++];
            code++;
            if (code == 0xFF) {
                dst[code_index] = code;
                code = 1;
                code_index = write_index++;
            }
        }
    }
    dst[code_index] = code;
    return write_index;
}

size_t cobsDecode(const uint8_t *ptr, size_t length, uint8_t *dst) {
    size_t read_index = 0, write_index = 0;
    uint8_t code, i;
    while (read_index < length) {
        code = ptr[read_index];
        if (read_index + code > length && code != 1) return 0;
        read_index++;
        for (i = 1; i < code; i++) dst[write_index++] = ptr[read_index++];
        if (code < 0xFF && read_index < length) dst[write_index++] = 0;
    }
    return write_index;
}

// Optional debug output on secondary UART
void printDebug(const char* msg) {
    Serial1.println(msg);
}

void sendFrame(uint8_t* cleartext, size_t len) {
    uint8_t cobs_buf[len + (len/254) + 2];
    size_t cobs_len = cobsEncode(cleartext, len, cobs_buf);
    
    Serial.write(0x00);
    Serial.write(cobs_buf, cobs_len);
    Serial.write(0x00);
}

void requestDtnTime() {
    printDebug("[*] Timesync request to ION...");
    uint8_t req[3];
    req[0] = UARTCL_VERSION_1 | UARTCL_FLAG_SYNC;
    uint16_t crc = computeCrc16(req, 1);
    req[1] = (crc >> 8) & 0xFF;
    req[2] = crc & 0xFF;
    sendFrame(req, 3);
}

uint32_t getLocalDtnTime() {
    return baseDtnTime + ((millis() - syncMillis) / 1000);
}

void sendPingBundle() {
    uint8_t bundle[] = {
        0x9F, // Indefinite Array
        0x88, 0x07, 0x00, 0x00, // Primary block
        0x83, 0x01, 0x01, 0x00, // Dst: ipn:1.0 
        0x83, 0x01, 0x01, 0x00, // Src: ipn:1.0
        0x83, 0x01, 0x01, 0x00, // Rpt: ipn:1.0
        0x82, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00, // Creation TS: Array[Time_32, Seq]
        0x1A, 0x00, 0x01, 0x51, 0x80, // Lifetime: 86400
        0x85, 0x01, 0x01, 0x00, 0x00, // Payload block
        0x4B, 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x55, 0x41, 0x52, 0x54, 0x21, // "Hello UART!" (11 byte)
        0xFF // Break
    };

    uint32_t currentT = getLocalDtnTime();
    
    // Offset 22: Timestamp
    bundle[22] = (currentT >> 24) & 0xFF;
    bundle[23] = (currentT >> 16) & 0xFF;
    bundle[24] = (currentT >> 8) & 0xFF;
    bundle[25] = currentT & 0xFF;

    // Offset 26: Sequence Number (Modulo 24 per mantenere l'encoding CBOR a 1 singolo byte)
    bundle[26] = (seqNum % 24); 
    seqNum++;

    size_t bundle_len = sizeof(bundle);
    uint8_t cleartext[bundle_len + 3];
    
    cleartext[0] = UARTCL_VERSION_1 | UARTCL_FLAG_DATA;
    memcpy(&cleartext[1], bundle, bundle_len);
    
    uint16_t crc = computeCrc16(cleartext, bundle_len + 1);
    cleartext[bundle_len + 1] = (crc >> 8) & 0xFF;
    cleartext[bundle_len + 2] = crc & 0xFF;

    sendFrame(cleartext, bundle_len + 3);
    
    Serial1.print("[+] Inviato PING Bundle Seq: ");
    Serial1.println(seqNum - 1);
}

void setup() {
    // La Seriale USB (Verso ION)
    Serial.begin(115200);  
    
    // La Seriale HW per il debug (Nessuna attesa bloccante!)
    Serial1.begin(115200); 

    // Aspettiamo solo la USB che è il cavo vitale
    while (!Serial) { delay(10); } 
    
    printDebug("\n=== muON-DTN UART-COBS Tester ===");

    // Richiede l'ora spaziale al gateway per innescare il loop
    requestDtnTime();
}

uint8_t rx_buffer[2048];
size_t rx_len = 0;

void loop() {
    // Legge asincronamente i byte in arrivo da ION via USB
    while (Serial.available()) {
        uint8_t b = Serial.read();
        
        if (b == 0x00) {
            if (rx_len > 0) {
                uint8_t decoded[2048];
                size_t decoded_len = cobsDecode(rx_buffer, rx_len, decoded);
                rx_len = 0; // Reset

                if (decoded_len >= 3) {
                    uint16_t rcv_crc = (decoded[decoded_len - 2] << 8) | decoded[decoded_len - 1];
                    uint16_t calc_crc = computeCrc16(decoded, decoded_len - 2);
                    
                    if (rcv_crc == calc_crc) {
                        // A. SYNC REPLY
                        if ((decoded[0] & UARTCL_FLAG_SYNC) == UARTCL_FLAG_SYNC) {
                            if (decoded_len == 7) { 
                                baseDtnTime = (decoded[1] << 24) | (decoded[2] << 16) | (decoded[3] << 8) | decoded[4];
                                syncMillis = millis();
                                timeSynced = true;
                                
                                Serial1.print("[+] Ricevuto Tempo DTN: ");
                                Serial1.println(baseDtnTime);
                                
                                // Innesca la reazione a catena
                                sendPingBundle();
                            }
                        } 
                        // B. DATA BUNDLE DA ION
                        else {
                            printDebug("[+] Ricevuto Bundle dati da ION! Rilancio...");
                            // Simula elaborazione ritardata (opzionale)
                            delay(500);
                            
                            // Reazione infinita (Ping Pong)
                            sendPingBundle();
                        }
                    } else {
                        printDebug("[-] Rilevato errore CRC!");
                    }
                }
            }
        } else {
            if (rx_len < sizeof(rx_buffer)) {
                rx_buffer[rx_len++] = b;
            } else {
                rx_len = 0; // Overflow
            }
        }
    }
}