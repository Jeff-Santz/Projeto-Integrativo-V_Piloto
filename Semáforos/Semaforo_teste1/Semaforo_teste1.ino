// ==========================================
// FIRMWARE: SEMÁFORO INTELIGENTE - ESP32 + W5500
// Versão: Arduino IDE Compatible
// ==========================================

#include <ArduinoJson.h>
#include <ESP32Time.h>
#include <SPI.h>
#include <Ethernet_Generic.h>

// ==========================================
// PINOS: SEMÁFORO
// ==========================================
#define PIN_OUT_VERDE    9
#define PIN_OUT_AMARELO  10
#define PIN_OUT_VERMELHO 11
#define PIN_IN_VERDE     6
#define PIN_IN_AMARELO   3
#define PIN_IN_VERMELHO  46

// ==========================================
// PINOS: SPI / W5500
// ==========================================
#define VSPI_SCK   15
#define VSPI_MISO  18
#define VSPI_MOSI  17
#define W5500_CS   16
#define SPI_FRQ    32000000

// ==========================================
// IDENTIFICAÇÃO E REDE
// ==========================================
#define ID_SEMAFORO    "SEMAFORO-01"
#define SOURCE_NODE     2
#define DEST_NODE       0

byte      mac[]    = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip       (192, 168, 10, 2);
IPAddress serverIP (192, 168, 10, 1);   // ← IP do servidor/controladora
const uint16_t serverPort = 5000;

// ==========================================
// VARIÁVEIS GLOBAIS DE ESTADO
// ==========================================
enum EstadoSemaforo { MODO_ROTINA, MODO_CRITICO };
volatile EstadoSemaforo estadoAtual = MODO_ROTINA;

TickType_t TEMPO_VERDE    = pdMS_TO_TICKS(10000);
TickType_t TEMPO_AMARELO  = pdMS_TO_TICKS(4000);
TickType_t TEMPO_VERMELHO = pdMS_TO_TICKS(16000);

String status_semaforo;


// ==========================================
// HANDLES FREERTOS
// ==========================================
TaskHandle_t TaskRotinaHandle  = NULL;
TaskHandle_t TaskStatusHandle  = NULL;
TaskHandle_t TaskRedeHandle    = NULL;

SemaphoreHandle_t xSinalFase;    // Binário — sinaliza mudança de fase
SemaphoreHandle_t xMutexRotina;  // Mutex — protege TEMPO_*
SemaphoreHandle_t xMutexClient;  // Mutex — protege acesso ao EthernetClient de envio

// ==========================================
// OBJETOS GLOBAIS
// ==========================================
ESP32Time     rtc;
EthernetClient client; 
EthernetClient tcpClient;           // Socket exclusivo de ENVIO

// ==========================================
// ASSINATURAS
// ==========================================
String montarJson(String id_semaforo);
void   atualizarFarol(int verde, int amarelo, int vermelho);
void   lerControladora(String jsonRecebido);
void   ROTINA(int tempo_verde, int tempo_amarelo, int tempo_vermelho);
void   CRITICO();
void   RESET();
void   sendPck(uint8_t sourceNode, uint8_t destinationNode, uint8_t state);

void taskRotina    (void* pvParameters);
void taskStatus    (void* pvParameters);
void taskEscutaRede(void* pvParameters);

// ==========================================
// FUNÇÕES DE CONTROLE DE ESTADO
// ==========================================

void CRITICO() {
    estadoAtual = MODO_CRITICO;
    Serial.println("[ESTADO] Transicao para MODO_CRITICO efetuada.");
    
    // Interrompe qualquer delay da taskRotina imediatamente
    if (TaskRotinaHandle != NULL) {
        xTaskNotifyGive(TaskRotinaHandle);
    }
}

void RESET() {
    estadoAtual = MODO_ROTINA;
    rtc.setTime(1767225600);
    Serial.println("[ESTADO] RESET efetuado.");
}

void ROTINA(int tempo_verde, int tempo_amarelo, int tempo_vermelho) {
    if (xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE) {
        TEMPO_VERDE    = pdMS_TO_TICKS(tempo_verde);
        TEMPO_AMARELO  = pdMS_TO_TICKS(tempo_amarelo);
        TEMPO_VERMELHO = pdMS_TO_TICKS(tempo_vermelho);
        xSemaphoreGive(xMutexRotina);
        Serial.println("[CONFIG] Novos tempos de sincronismo aplicados.");
    }
}

void lerControladora(String jsonRecebido) {
    JsonDocument inputDoc;
    DeserializationError erro = deserializeJson(inputDoc, jsonRecebido);
    if (erro) return;

    // Comando de Ação
    if (inputDoc.containsKey("mensagem")) {
        String msg = inputDoc["mensagem"];
        if (msg == "RESET") RESET();
        else if (msg == "CRITICO") CRITICO();
    }
    
    // Atualização de Tempo
    if (inputDoc.containsKey("timestamp")) {
        long long novoTempo = inputDoc["timestamp"];
        rtc.setTime(novoTempo);
        Serial.print("[SYNC] Relógio atualizado para: ");
        Serial.println(novoTempo);
    }
}

// ==========================================
// ENVIO TCP — orientado a eventos
// Chamada exclusivamente pela taskStatus
// ==========================================

void sendPck(String payload) {
    // Pega o Mutex para não escrever no SPI enquanto a thread de escuta estiver lendo
    if (xSemaphoreTake(xMutexClient, pdMS_TO_TICKS(500)) != pdTRUE) {
        Serial.println("[REDE] sendPck: timeout do mutex SPI.");
        return;
    }

    if (tcpClient.connected()) {
        tcpClient.println(payload);
        Serial.print("[REDE] Payload enviado via TCP: ");
        Serial.println(payload);
    } else {
        Serial.println("[REDE] sendPck: Ignorado. Link TCP esta down.");
    }

    xSemaphoreGive(xMutexClient);
}

// ==========================================
// THREADS FREERTOS
// ==========================================

/**
 * taskRotina — Núcleo 1, Prio 1
 * Controla o ciclo de fases do semáforo.
 * Em MODO_CRITICO pisca o amarelo continuamente.
 */
void taskRotina(void* pvParameters) {
    Serial.print("[THREAD] Rotina iniciada no nucleo: ");
    Serial.println(xPortGetCoreID());

    for (;;) {
        if (estadoAtual == MODO_CRITICO) {
            atualizarFarol(0, 1, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            atualizarFarol(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // --- FASE: VERMELHO ---
        if (xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE) {
            atualizarFarol(0, 0, 1);
            xSemaphoreGive(xSinalFase);
            xSemaphoreGive(xMutexRotina);
        }
        if (ulTaskNotifyTake(pdTRUE, TEMPO_VERMELHO) > 0) continue;

        // --- FASE: VERDE ---
        if (estadoAtual == MODO_ROTINA &&
            xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE)
        {
            atualizarFarol(1, 0, 0);
            xSemaphoreGive(xSinalFase);
            xSemaphoreGive(xMutexRotina);
            if (ulTaskNotifyTake(pdTRUE, TEMPO_VERDE) > 0) continue;
        }

        // --- FASE: AMARELO ---
        if (estadoAtual == MODO_ROTINA &&
            xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE)
        {
            atualizarFarol(0, 1, 0);
            xSemaphoreGive(xSinalFase);
            xSemaphoreGive(xMutexRotina);
            if (ulTaskNotifyTake(pdTRUE, TEMPO_AMARELO) > 0) continue;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * taskStatus — Núcleo 1, Prio 1
 * Aguarda xSinalFase. A cada mudança de fase,
 * monta o JSON de status e dispara sendPck() via TCP.
 */
void taskStatus(void* pvParameters) {
    Serial.print("[THREAD] Status iniciada no nucleo: ");
    Serial.println(xPortGetCoreID());

    for (;;) {
        if (xSemaphoreTake(xSinalFase, portMAX_DELAY) == pdTRUE) {
            status_semaforo = montarJson(ID_SEMAFORO);
            sendPck(status_semaforo);
        }
    }
}

/**
 * taskEscutaRede — Núcleo 0, Prio 2
 * Mantém conexão TCP de entrada para receber comandos
 * da controladora. Reconecta automaticamente se cair.
 * Usa socket próprio (clientRx) separado do de envio.
 */
void taskEscutaRede(void* pvParameters) {
    Serial.print("[THREAD] EscutaRede iniciada no nucleo: ");
    Serial.println(xPortGetCoreID());

    char buffer[512];

    for (;;) {
        // 1. Gerenciamento da Conexão
        if (!tcpClient.connected()) {
            Serial.println("[REDE] EscutaRede: reconectando ao servidor Master...");
            // --- GATILHO FAILSAFE ---
            // if (estadoAtual != MODO_CRITICO) {
            //     Serial.println("[FAILSAFE] TCP Desconectado. Forcando MODO_CRITICO imediato.");
            //     CRITICO();
            // }
            tcpClient.stop();

            if (!tcpClient.connect(serverIP, serverPort)) {
                Serial.println("[REDE] EscutaRede: falha — nova tentativa em 3s.");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            Serial.println("[REDE] EscutaRede: conexao ESTABELECIDA.");
        }

        // 2. Leitura de Dados (Protegida pelo Mutex SPI)
        if (tcpClient.available()) {
            if (xSemaphoreTake(xMutexClient, pdMS_TO_TICKS(100)) == pdTRUE) {
                int len = 0;
                while (tcpClient.available() && len < (int)(sizeof(buffer) - 1)) {
                    buffer[len++] = tcpClient.read();
                }
                buffer[len] = '\0';
                xSemaphoreGive(xMutexClient);

                Serial.print("[REDE] Payload recebido: ");
                Serial.println(buffer);
                
                lerControladora(String(buffer));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // Libera a CPU do núcleo 0
    }
}
// ==========================================
// FUNÇÕES AUXILIARES
// ==========================================

String montarJson(String id_semaforo) {
    JsonDocument doc;
    doc["id_semaforo"]        = id_semaforo;
    doc["timestamp"]          = rtc.getTime("%d/%m/%Y %H:%M:%S");
    doc["status"]["vermelho"] = digitalRead(PIN_IN_VERMELHO);
    doc["status"]["amarelo"]  = digitalRead(PIN_IN_AMARELO);
    doc["status"]["verde"]    = digitalRead(PIN_IN_VERDE);

    String output;
    serializeJson(doc, output);

    Serial.println("\n--- [EVENTO] Nova Fase ---");
    Serial.print("JSON: ");
    Serial.println(output);

    return output;
}

void atualizarFarol(int verde, int amarelo, int vermelho) {
    digitalWrite(PIN_OUT_VERDE,    verde);
    digitalWrite(PIN_OUT_AMARELO,  amarelo);
    digitalWrite(PIN_OUT_VERMELHO, vermelho);
}

// ==========================================
// SETUP
// ==========================================

void setup() {
    Serial.begin(115200);
    Serial.println("=== Inicializando Sistema de Semaforo Inteligente ===");

    // --- Pinos do Semáforo ---
    pinMode(PIN_OUT_VERDE,    OUTPUT);
    pinMode(PIN_OUT_AMARELO,  OUTPUT);
    pinMode(PIN_OUT_VERMELHO, OUTPUT);
    pinMode(PIN_IN_VERDE,     INPUT);
    pinMode(PIN_IN_AMARELO,   INPUT);
    pinMode(PIN_IN_VERMELHO,  INPUT);

    // --- Hardware Ethernet W5500 ---
    // SPI deve ser inicializado ANTES de Ethernet.init()
    SPI.begin(VSPI_SCK, VSPI_MISO, VSPI_MOSI, W5500_CS);
    SPI.setFrequency(SPI_FRQ);

    Ethernet.init(W5500_CS);
    Serial.println("[REDE] Inicializando Ethernet W5500...");
    Ethernet.begin(mac, ip);
    delay(1000);  // aguarda link físico estabilizar
    Serial.print("[REDE] IP local: ");
    Serial.println(Ethernet.localIP());

    // --- RTC interno ---
    rtc.setTime(1767225600);

    // --- Primitivas FreeRTOS ---
    xSinalFase   = xSemaphoreCreateBinary();
    xMutexRotina = xSemaphoreCreateMutex();
    xMutexClient = xSemaphoreCreateMutex();

    // --- Criação das Threads ---
    // Rede isolada no núcleo 0 — não interfere no timing de controle
    xTaskCreatePinnedToCore(taskEscutaRede, "EscutaRede", 8192, NULL, 2, &TaskRedeHandle,   0);

    // Controle e status no núcleo 1
    xTaskCreatePinnedToCore(taskRotina,     "Rotina",     4096, NULL, 1, &TaskRotinaHandle, 1);
    xTaskCreatePinnedToCore(taskStatus,     "Status",     8192, NULL, 1, &TaskStatusHandle, 1);

    Serial.println("[SETUP] Threads iniciadas. Sistema operacional.");
}

// ==========================================
// LOOP — ocioso (toda lógica nas threads)
// ==========================================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
