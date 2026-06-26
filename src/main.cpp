#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LittleFS.h>

// --- Configurações Wi-Fi e Servidor TCP ---
const char* ssid = "TERRAPLANISMO";
const char* password = "terraplanismo_adm";
WiFiServer server(3333);

// --- Configurações NFC ---
#define RST_PIN 22
#define SS_PIN 5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// --- Configurações Servos ---
const int servoPins[4] = {13, 12, 14, 27};
Servo servos[4];

// --- Estrutura da Máquina de Estados ---
enum SystemState { STATE_IDLE, STATE_RECORD_GET_PLANET, STATE_RECORD_WAIT_NFC, STATE_READING_NFC };
SystemState currentState = STATE_IDLE;

String requestedPlanet = "";
String targetRecordPlanet = "";
int failedAttempts = 0;
bool testNfcServoMode = false;
unsigned long readTimeoutStart = 0;

// --- Protótipos Essenciais ---
void processCommand(String cmd, WiFiClient* client);
void handleNFC();
void handleSerialMonitor();
void executeServoAction(int servoIndex, int angle);
void tcpServerTask(void *pvParameters);

void setup() {
    Serial.begin(115200);

    // 1. Inicializa Sistema de Arquivos Flash
    if (!LittleFS.begin(true)) {
        Serial.println("Erro ao montar partição LittleFS.");
    }

    // 2. Acopla e centraliza os Servos
    for (int i = 0; i < 4; i++) {
        servos[i].setPeriodHertz(50);
        servos[i].attach(servoPins[i], 500, 2500);
        servos[i].write(90); 
    }
    servos[2].write(0); 
    servos[3].write(0);

    // 3. Inicializa SPI e RC522
    SPI.begin();
    mfrc522.PCD_Init();

    // 4. Inicia AP Mode
    WiFi.softAP(ssid, password);
    server.begin();
    Serial.println("Wi-Fi AP Iniciado | Servidor TCP escutando na porta 3333");

    // 5. Destaca o servidor TCP em uma Task exclusiva de rede
    xTaskCreate(tcpServerTask, "TCP_Task", 4096, NULL, 1, NULL);
}

void loop() {
    handleSerialMonitor(); // Escuta teclado/terminal
    handleNFC();           // Escuta aproximação de tags
    
    // Processamento do Timeout de Busca NFC (30 segundos)
    if (currentState == STATE_READING_NFC && (millis() - readTimeoutStart > 30000)) {
        Serial.println("[TIMEOUT] Busca NFC expirou.");
        currentState = STATE_IDLE;
        failedAttempts = 0;
    }
    delay(50); // Delay suave para ceder tempo à task idle do processador
}

// =========================================================
//                   TAREFA DE REDE (TCP)
// =========================================================
void tcpServerTask(void *pvParameters) {
    while (true) {
        WiFiClient client = server.available();
        if (client) {
            client.setNoDelay(true); // Reduz latência TCP
            while (client.connected()) {
                if (client.available()) {
                    String req = client.readStringUntil('\n');
                    req.trim();
                    if (req.length() > 0) processCommand(req, &client);
                }
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            client.stop();
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =========================================================
//               ROTEAMENTO DE COMANDOS STRING
// =========================================================
void processCommand(String cmd, WiFiClient* client) {
    cmd.toUpperCase();
    String resp = "";

    if (cmd.startsWith("BUSCA:")) {
        if (currentState != STATE_IDLE) { 
            resp = "ERRO:ESP32 Ocupado\n"; 
        } else {
            requestedPlanet = cmd.substring(6);
            currentState = STATE_READING_NFC;
            readTimeoutStart = millis();
            failedAttempts = 0;
            resp = "BUSCANDO:" + requestedPlanet + "\n";
        }
    } else if (cmd == "STATUS") {
        resp = "STATUS:estado=" + String(currentState) + ",modo_combinado=" + (testNfcServoMode ? "ON" : "OFF") + "\n";
    } else if (cmd == "LISTAR") {
        File f = LittleFS.open("/planetas.txt", "r");
        if (f) { resp = "LISTAR:\n" + f.readString(); f.close(); }
    } else if (cmd == "LIMPAR") {
        LittleFS.remove("/planetas.txt");
        resp = "Base de dados limpa.\n";
    } else if (cmd.startsWith("TESTE_SERVO")) {
        int idx = cmd.substring(11).toInt() - 1; // Extrai número e ajusta para índice (0-3)
        if (idx >= 0 && idx < 4) {
            executeServoAction(idx, 0); delay(1000); 
            executeServoAction(idx, 180); delay(1000); 
            executeServoAction(idx, 90);
            resp = cmd + "_CONCLUIDO\n";
        }
    } else if (cmd == "OPEN") {
        servos[2].write(0); servos[3].write(180); delay(3000);
        servos[2].write(90); servos[3].write(90);
        resp = "TESTE_PAR_OPEN_CONCLUIDO\n";
    } else if (cmd == "TIRA") {
        servos[2].write(180); servos[3].write(0); delay(3000);
        servos[2].write(90); servos[3].write(90);
        resp = "TESTE_PAR_TIRA_CONCLUIDO\n";
    } else { 
        resp = "COMANDO_INVALIDO\n"; 
    }

    if (client) client->print(resp); // Retorna via Socket
    Serial.print(resp);              // Espelha no monitor
}

// =========================================================
//               GERENCIADOR DO MONITOR SERIAL
// =========================================================
void handleSerialMonitor() {
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        input.toUpperCase();

        if (input == "GRAVAR") {
            currentState = STATE_RECORD_GET_PLANET;
            Serial.println("\n[MODO GRAVACAO] Introduza o nome do planeta: ");
        } else if (input == "END") {
            currentState = STATE_IDLE;
            testNfcServoMode = false;
            Serial.println("\nModo normal restaurado.");
        } else if (currentState == STATE_RECORD_GET_PLANET) {
            targetRecordPlanet = input;
            currentState = STATE_RECORD_WAIT_NFC;
            Serial.println("\nPlaneta: " + targetRecordPlanet + "\nAproxime a tag NFC...");
        } else {
            processCommand(input, NULL);
        }
    }
}

// =========================================================
//                     LEITURA NFC
// =========================================================
void handleNFC() {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

    // Converte Bytes do UID em String Hexadecimal
    String uidStr = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) uidStr += "0";
        uidStr += String(mfrc522.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase();
    mfrc522.PICC_HaltA(); // Trava leitura sequencial desenfreada da mesma tag

    if (testNfcServoMode) {
        Serial.println("\n[TESTE NFC+SERVO] Tag detectada: " + uidStr);
        executeServoAction(0, 0); delay(1000); executeServoAction(0, 90);
        return;
    }

    // Rotina de Gravação de Cartão Novo
    if (currentState == STATE_RECORD_WAIT_NFC) {
        File f = LittleFS.open("/planetas.txt", "a");
        if (f) {
            f.println(targetRecordPlanet + "," + uidStr);
            f.close();
            Serial.println("\n[GRAVADO] Tag " + uidStr + " -> " + targetRecordPlanet);
            Serial.println("Envie 'END' para sair ou cadastre outro planeta.");
        }
    } 
    // Rotina de Validação de Cartão 
    else if (currentState == STATE_READING_NFC) {
        File f = LittleFS.open("/planetas.txt", "r");
        bool found = false;
        String detectedPlanet = "";
        
        while (f && f.available()) {
            String line = f.readStringUntil('\n');
            int commaIdx = line.indexOf(',');
            if (commaIdx > 0 && line.substring(commaIdx + 1).indexOf(uidStr) != -1) {
                detectedPlanet = line.substring(0, commaIdx);
                detectedPlanet.toUpperCase();
                found = true; 
                break;
            }
        }
        if (f) f.close();

        if (found && detectedPlanet == requestedPlanet) {
            Serial.println("\n[NFC OK] Tag " + uidStr + " confirmada para " + detectedPlanet);
            currentState = STATE_IDLE; // Destrava sistema
        } else {
            failedAttempts++;
            if (failedAttempts >= 3) {
                Serial.println("\n[BLOQUEIO] 3 tentativas falhas. Abortando...");
                currentState = STATE_IDLE;
            }
        }
    }
}

// Wrapper para enxugar a sintaxe de atuação dos Servos
void executeServoAction(int servoIndex, int angle) {
    servos[servoIndex].write(angle);
}