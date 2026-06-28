# Projeto ESP32 Gamification: Validador NFC e Controle de Servomotores

Este projeto consiste em um sistema interativo desenvolvido para o microcontrolador ESP32 utilizando a plataforma PlatformIO. O sistema opera como uma máquina de estados que gerencia um jogo/desafio (gamification), no qual o usuário precisa aproximar tags RFID/NFC específicas (representando planetas) para avançar de fase e acionar respostas físicas através de servomotores.

## 🛠️ Tecnologias e Bibliotecas

O projeto foi construído utilizando o framework Arduino e requer as seguintes bibliotecas gerenciadas automaticamente pelo PlatformIO:
* **MFRC522** (v1.4.11 - miguelbalboa): Leitura e controle do módulo RFID/NFC.
* **ESP32Servo** (v3.0.5 - madhephaestus): Controle via hardware PWM dos servomotores.
* **Bibliotecas nativas:** `WiFi.h` e `SPI.h`.

## 🔌 Pinagem e Hardware

| Componente | Pino ESP32 | Descrição |
| :--- | :--- | :--- |
| **MFRC522 (SDA/SS)** | `GPIO 5` | Seleção do módulo RFID. |
| **MFRC522 (RST)** | `GPIO 22` | Reset do módulo RFID. |
| **Servo 1** | `GPIO 13` | Acionado através do Timer 0. |
| **Servo 2** | `GPIO 12` | Acionado através do Timer 1. |
| **Servo 3** | `GPIO 14` | Acionado através do Timer 2. |
| **Servo 4** | `GPIO 27` | Acionado através do Timer 3. |

*Os servos operam em 50Hz, inicializando centralizados em 90° e com pulsos calibrados entre 500us e 2400us*.

## 📡 Configurações de Rede e Comunicação

O ESP32 cria seu próprio Ponto de Acesso (Access Point) e hospeda um servidor TCP para se comunicar com um cliente (como um aplicativo ou outro sistema de controle).

* **SSID (Rede):** `TERRAPLANISMO`
* **Senha:** `terraplanismo_adm`
* **Porta TCP:** `3333`

## ⚙️ Funcionamento da Lógica e Máquina de Estados

O sistema alterna primariamente entre dois estados: `STATE_IDLE` (ocioso) e `STATE_READING_NFC` (aguardando leitura). 

1. **Início da Fase:** O servidor TCP recebe um comando iniciando com `busca:` ou `planet_selected`, seguido pelo nome do planeta (ex: `busca: MARTE`). O sistema entra no modo de leitura e aguarda o NFC.
2. **Validação:** Ao ler um cartão, o sistema compara o UID (identificador único) com um banco de dados estático que associa UIDs a planetas (ex: "FF0FBA317F0100" = MERCURIO).
3. **Sucesso:** Se o planeta lido for o mesmo solicitado, o ESP envia a mensagem TCP `answer_correct`. Após completar as fases, ativa uma animação que movimenta os servos 3 e 4.
4. **Erro:** Em caso de tag divergente, a mensagem `answer_incorrect` é enviada e os servos 3 e 4 se movimentam momentaneamente. Se houver acúmulo de **3 erros consecutivos** ou o tempo limite (Timeout) esgotar, uma animação de punição movimenta múltiplos servos e reseta a contagem.
5. **Timeout:** O sistema possui uma trava de segurança baseada em tempo (`TIMEOUT_MS = 180000`), enviando `answer_timeout` caso a leitura demore.

## 💻 Comandos Suportados

### Comandos de Entrada TCP
* `busca: [NOME_DO_PLANETA]` ou `planet_selected [NOME_DO_PLANETA]`: Define o planeta alvo, forçando os servos à posição original (90°) e habilitando a leitura NFC.

### Comandos do Monitor Serial (Baud Rate: 115200)
Úteis para depuração técnica e testes físicos:
* `TESTE_SERVO [1-4]`: Testa o curso completo (0° -> 180° -> 90°) de um servo específico. Exemplo: `TESTE_SERVO 1`.
* `TESTE_PARES`: Executa uma coreografia de teste com os pares de servos (3/4 e depois 1/2).
* `RESET`: Aborta todas as animações, zera os contadores de erro, coloca o estado em IDLE e força todos os servos para 90°.
* `TESTE_NFC`: Habilita o leitor passivamente apenas para imprimir no console o UID (em formato Hexadecimal) de qualquer tag que for aproximada, facilitando o cadastro de novas tags.
