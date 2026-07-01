#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

// =========================================================
//            CONFIGURAÇÕES GERAIS E BANCO ESTÁTICO
// =========================================================

const char* ssid = "TERRAPLANISMO";
const char* password = "terraplanismo_adm";
WiFiServer server(3333);
WiFiClient activeClient; 

#define RST_PIN 22
#define SS_PIN 5
#define buttons_PIN 32

MFRC522 mfrc522(SS_PIN, RST_PIN);
                     //  {1  , 2, 3,   4}
const int servoPins[4] = {13, 14, 26, 27};
//servo[0] = servo porta
//servo[1] = servo esquerda 
//servo[2] = servo esquerda 
//servo[3] = servo direita
Servo servos[4];

//int cont_fases = 0;

struct PlanetTag {
    String planet;
    String uid;
};

// Base de Dados Estática
PlanetTag planetDB[] = {
    {"MERCURIO", "FF0F91327F0100"},
    {"VENUS",    "FF0F03327F0100"},
    {"TERRA",    "FF0F02327F0100"}, 
    {"TERRA",    "FF0F48327F0100"}, 
    {"MARTE",    "FF0F01327F0100"},
    {"JUPITER",  "FF0F8E327F0100"},
    {"SATURNO",  "FF0F4B327F0100"},
    {"URANO",    "FF0F8F327F0100"},
    {"NETUNO",   "FF0FD6327F0100"},
    {"SOL",      "FF0FBD317F0100"},
    {"LUA",      "FF0FBB317F0100"},
    {"MARTE",    "FF0F04327F0100"},
    {"MARTE",    "FF0F4A327F0100"},
};
const int dbSize = sizeof(planetDB) / sizeof(planetDB[0]);

// =========================================================
//                MÁQUINA DE ESTADOS E GAMIFICATION
// =========================================================
enum SystemState { STATE_IDLE, STATE_READING_NFC, STATE_READING_BUTTONS, STATE_TUTORIAL };
SystemState currentState = STATE_IDLE;

String requestedPlanet = "";
unsigned long readTimeoutStart = 0;
const unsigned long TIMEOUT_MS = 180000; // 15 Segundos

// Controle de Animação Rápida dos Servos (Tempo apenas para completar o curso)
unsigned long servoSuccessTimer = 0;
bool servoSuccessActive = false;

unsigned long servoErrorTimer = 0;
bool servoErrorActive = false;

const unsigned long TRANSIT_TIME_MS = 400; // Tempo físico para o braço girar antes de voltar

// --- Protótipos ---
void processTCPCommand(String cmd);
void sendTCPMessage(String msg);
void sendTCPMessageFast(String msg);
void handleNFC();
void handleSerialMonitor();
void executeServoAction(int servoIndex, int angle);
void tcpServerTask(void *pvParameters);
void triggerThreeErrorsAnimation();
void button_module(void *pvParameters);

// =========================================================
//                        SETUP
// =========================================================
void setup() {
    Serial.begin(115200);

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);

    Serial.println("[SERVO-LOG] Inicializando PWM de Hardware de forma permanente...");
    for (int i = 0; i < 4; i++) {
        servos[i].setPeriodHertz(50); 
        servos[i].attach(servoPins[i], 500, 2400); 
        servos[i].write(90); // Centraliza em 90°
    }

    SPI.begin();
    mfrc522.PCD_Init();

    WiFi.softAP(ssid, password);
    server.begin();
    Serial.println("\n[SISTEMA] Sistema Pronto | Espera de 5s Removida.");
    
    xTaskCreate(tcpServerTask, "TCP_Task", 4096, NULL, 1, NULL);
    xTaskCreate(button_module, "Button_Task", 2048, NULL, 1, NULL);
}

// =========================================================
//                        LOOP
// =========================================================
void loop() {
    handleSerialMonitor();
    handleNFC(); 

    // Timeout de 15s na busca NFC
    if (currentState == STATE_READING_NFC && (millis() - readTimeoutStart > TIMEOUT_MS)) {
        Serial.println("\n[TIMEOUT-LOG] 15 segundos esgotados sem leitura de tag.");
        sendTCPMessage("answer_timeout"); 
        
    }

    // --- RETORNO AUTOMÁTICO IMEDIATO DOS SERVOS ---
    
    // Retorno dos Servos 3 e 4 logo após atingirem 0°
    if (servoSuccessActive && (millis() - servoSuccessTimer >= TRANSIT_TIME_MS)) {
        Serial.println("[SERVO-LOG] Curso concluido: Retornando servos 3 e 4 para 90°");
        servos[2].write(90);
        servos[3].write(90); 
        delay(600);
        servoSuccessActive = false;
    }

    // Retorno de todos os servos logo após atingirem 180° (3 Erros)
    if (servoErrorActive && (millis() - servoErrorTimer >= TRANSIT_TIME_MS)) {
        Serial.println("[SERVO-LOG] Curso concluido: Retornando todos os servos para 90°");
        for (int i = 0; i < 4; i++) servos[i].write(90);
        delay(600);
        servoErrorActive = false;
    }

    delay(15);
}

// =========================================================
//                  LÓGICA MECÂNICA
// =========================================================

void triggerThreeErrorsAnimation() {
    Serial.println("\n[SERVO-LOG] 3 erros consecutivos! Movendo servos 1, 2, 3 e 4 para 180°");
    if (currentState != STATE_TUTORIAL) {
        Serial.println("\n[SERVO-LOG] ALERTA: 3 erros! Movendo servos 1, 2, 3 e 4 para 180°");
        //codigo pra abrir a porta
        servos[0].write(180);
        delay(5000);
        servos[0].write(90);
        delay(600);
        servoErrorTimer = millis();
        servoErrorActive = true;
        servoSuccessActive = false; 
        
    }
}

void executeServoAction(int servoIndex, int angle) {
    servos[servoIndex].write(angle); delay(600);
}

// =========================================================
//                     LEITURA NFC
// =========================================================
void handleNFC() {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

    String uidStr = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) uidStr += "0";
        uidStr += String(mfrc522.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase();
    
    mfrc522.PICC_HaltA(); 
    mfrc522.PCD_StopCrypto1();

    if ((currentState == STATE_READING_NFC) || (currentState == STATE_TUTORIAL)) {
        String detectedPlanet = "";
        bool found = false;
        
        for (int i = 0; i < dbSize; i++) {
            if (planetDB[i].uid == uidStr) {
                detectedPlanet = planetDB[i].planet;
                found = true;
                break;
            }
        }
        
        // --- CASO 1: LEITURA CORRETA ---
        if (found && detectedPlanet == requestedPlanet) {
            Serial.println("\n[NFC-LOG] Sucesso! Tag " + uidStr + " validada para o planeta " + detectedPlanet);
            sendTCPMessage("answer_correct");
            
            
            if (currentState != STATE_TUTORIAL) {
                Serial.println("[SERVO-LOG] Resposta Correta: Pulsando servos 3 e 4 para abrir a tampa");
                servos[2].write(0);
                servos[3].write(180);
                delay(2000);
                servoSuccessTimer = millis();
                servoSuccessActive = true;
                servoErrorActive = false;
                //currentState = STATE_IDLE; // Desbloqueia instantaneamente para receber a próxima fase
            }
        } 
        // --- CASO 2: LEITURA INCORRETA ---
        else {
            Serial.println("\n[NFC-LOG] Erro! Tag detectada (" + uidStr + ") nao condiz.");
            sendTCPMessage("answer_incorrect");
            if (currentState != STATE_TUTORIAL) {
                Serial.println("[SERVO-LOG] Resposta Incorreta: Pulsando servos 3 e 4 para tirar o planeta");
                servos[2].write(150);
                servos[3].write(30);
                delay(600);
                servos[2].write(90);
                servos[3].write(90);
                delay(100);
                
            }
        }
    }
}

// =========================================================
//                   TAREFA DE REDE (TCP)
// =========================================================
void tcpServerTask(void *pvParameters) {
    Serial.println("[TCP-LOG] Tarefa TCP iniciada. Aguardando conexões...");
    while (true) {
        WiFiClient newClient = server.available();
        if (newClient) {
            // cont_fases = 0;
            Serial.println("\n[TCP-LOG] Cliente conectado! IP: " + newClient.remoteIP().toString());
            if (activeClient && activeClient.connected()) activeClient.stop();
            activeClient = newClient;
            activeClient.setNoDelay(true);

            while (activeClient.connected()) {
                if (activeClient.available()) {
                    String req = activeClient.readStringUntil('\n');
                    req.trim();
                    if (req.length() > 0) {
                        processTCPCommand(req);
                    }
                }
                vTaskDelay(5 / portTICK_PERIOD_MS);
            }
            Serial.println("[TCP-LOG] Cliente desconectado.");
            activeClient.stop();

            // Cliente caiu/desconectou: encerra qualquer leitura de botões em andamento
            if (currentState != STATE_IDLE) {
                currentState = STATE_IDLE;
                Serial.println("[BOTOES-LOG] Estado de leitura de botões encerrado (cliente desconectou).");
            }
        }
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}

// Envio "pesado": usado pelo NFC. Mantém o delay propositalmente,
// para dar tempo do pico eletrico dos servos se dissipar antes do próximo envio.
void sendTCPMessage(String msg) {
    if (activeClient && activeClient.connected()) {
        activeClient.println(msg);
        Serial.println("[TCP-LOG] Mensagem Enviada -> " + msg);
        delay(2000);
    }
}

// Envio "rápido": usado pelos botões. Sem delay, para garantir resposta em tempo real,
// já que a leitura de botões não aciona servos.
void sendTCPMessageFast(String msg) {
    if (activeClient && activeClient.connected()) {
        activeClient.println(msg);
    }
}

void main_force_servos_home() {
    // Interrompe animações antigas e força retorno a 90° imediatamente se uma nova fase entrar
    if (servoSuccessActive || servoErrorActive) {
        delay(100); // Pequena pausa para garantir que o movimento anterior seja interrompido
        Serial.println("[SERVO-LOG] Nova fase recebida! Forçando interrupcao de movimento e reset para 90°");
        for (int i = 0; i < 4; i++) servos[i].write(90);
        servoSuccessActive = false;
        servoErrorActive = false;
    }
}

void processTCPCommand(String cmd) {
    String lowerCmd = cmd;
    lowerCmd.toLowerCase();

    if (lowerCmd.startsWith("busca:") || lowerCmd.startsWith("planet_selected")) {
        // cont_fases++;
        int separatorIdx = cmd.indexOf(':');
        if (separatorIdx == -1) separatorIdx = cmd.indexOf(' '); 
        
        if (separatorIdx != -1) {
            requestedPlanet = cmd.substring(separatorIdx + 1);
            requestedPlanet.trim();
            requestedPlanet.toUpperCase();
        }
        
        main_force_servos_home(); // Garante o alinhamento total de forma imediata
        currentState = STATE_READING_NFC;
        readTimeoutStart = millis(); 
        Serial.println("[SISTEMA] Nova Fase ativa. Aguardando tag para: " + requestedPlanet);
    }

    else if((lowerCmd.startsWith("game_over")) || (lowerCmd == "game_over")) {
        Serial.println("[SISTEMA] Game Over recebido. Resetando sistema...");
        currentState = STATE_IDLE;
        delay(3500);
        triggerThreeErrorsAnimation(); // Animação de erro final 
    }

    // --- Ativa o estado de leitura de botões ---
    else if (lowerCmd.startsWith("botoes:on") || lowerCmd == "buttons_on") {
        currentState = STATE_READING_BUTTONS;
        Serial.println("[BOTOES-LOG] Estado de leitura de botões ATIVADO.");
    }
    // --- Encerra o estado de leitura de botões ---
    else if (lowerCmd.startsWith("botoes:off") || lowerCmd == "buttons_off") {
        if (currentState == STATE_READING_BUTTONS) {
            currentState = STATE_IDLE;
        }
        Serial.println("[BOTOES-LOG] Estado de leitura de botões DESATIVADO.");
    }
    else if (lowerCmd.startsWith("tutorial:on") || lowerCmd == "tutorial_on") {
        currentState = STATE_TUTORIAL;
        Serial.println("[TUTORIAL-LOG] Modo Tutorial ATIVADO.");
    }
    else if (lowerCmd.startsWith("tutorial:off") || lowerCmd == "tutorial_off") {
        triggerThreeErrorsAnimation(); 
        currentState = STATE_IDLE;
        Serial.println("[TUTORIAL-LOG] Modo Tutorial DESATIVADO.");
    }
}

// =========================================================
//               GERENCIADOR DO MONITOR SERIAL
// =========================================================
void handleSerialMonitor() {
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        input.toUpperCase();

        if (input.startsWith("TESTE_SERVO")) {
            int idx = -1;
            if (input.indexOf(' ') != -1) idx = input.substring(input.indexOf(' ') + 1).toInt() - 1;
            if (idx >= 0 && idx < 4) {
                Serial.printf("\n[TESTE] Movendo Servo %d...\n", idx + 1);
                servos[idx].write(0); delay(600);
                servos[idx].write(180); delay(600);
                servos[idx].write(90);
                Serial.println("[TESTE] Concluido.");
            }
        }
        if (input.startsWith("TESTE_PARES")) {
            servos[2].write(60);
            servos[3].write(120);
            delay(600);
            servos[2].write(90);
            servos[3].write(90);
            delay(1000);
            servos[1].write(120);
            servos[0].write(60);
            delay(600);
            servos[1].write(90);
            servos[0].write(90);
            delay(600);
        }
        if (input == "RESET") {
            Serial.println("\n[RESET] Resetando sistema e servos para 90°...");
            for (int i = 0; i < 4; i++) servos[i].write(90);
            currentState = STATE_IDLE;
            
            servoSuccessActive = false;
            servoErrorActive = false;
            Serial.println("[RESET] Concluido.");
        }
        if (input == "TESTE_NFC") {
            if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

            String uidStr = "";
            for (byte i = 0; i < mfrc522.uid.size; i++) {
                if (mfrc522.uid.uidByte[i] < 0x10) uidStr += "0";
                uidStr += String(mfrc522.uid.uidByte[i], HEX);
            }
            uidStr.toUpperCase();
            
            mfrc522.PICC_HaltA(); 
            mfrc522.PCD_StopCrypto1();
            Serial.println("[TESTE_NFC] Leitura concluída. UID: " + uidStr);
        }
    }
}

// =========================================================
//            Task Leitura do Módulo de Botões
// =========================================================
void button_module(void *pvParameters) {
    pinMode(buttons_PIN, INPUT_PULLUP);
    int key_read;
    String Key_id, Key_id_prev = "";
    unsigned long lastChangeTime = 0;
    const unsigned long DEBOUNCE_MS = 150; 

    while (true) {
        if (currentState == STATE_READING_BUTTONS) {
            key_read = analogRead(buttons_PIN);
            
            // Mantive seus valores de calibração originais
            if (key_read == 0) {
                Key_id = "LEFT";
            } else if (key_read > 10 && key_read < 500) {
                Key_id = "UP";
            } else if (key_read > 600 && key_read < 1100) {
                Key_id = "DOWN";
            } else if (key_read > 1600 && key_read < 1900) {
                Key_id = "RIGHT";
            } else if (key_read > 2800 && key_read < 3100) {
                Key_id = "OK";
            } else {
                Key_id = ""; // Nenhum botão pressionado
            }

            unsigned long now = millis();

            // Verifica se o botão mudou de estado E se o tempo de debounce já passou
            if (Key_id != Key_id_prev && (now - lastChangeTime >= DEBOUNCE_MS)) {
                
                // 1. Se um botão foi PRESSIONADO
                if (Key_id.length() > 0 ) {
                    sendTCPMessageFast(Key_id);
                }
                // 2. Se o botão foi SOLTO (Key_id atual é vazio, mas o anterior não era)
                 if (Key_id != Key_id_prev) {
                    delay(DEBOUNCE_MS);
                }

                // Atualiza o estado anterior e reseta o temporizador
                Key_id_prev = Key_id;
                //lastChangeTime = now;
            }
        } 
        else {
            // Se o minigame acabar enquanto o jogador estiver segurando um botão,
            // garante que enviamos um STOP e limpamos o estado para não bugar a nave depois.
            if (Key_id_prev.length() > 0) {
                sendTCPMessageFast("STOP");
                Key_id_prev = "";
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}