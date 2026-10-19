#include <Arduino.h>
#include <ArduinoJson.h> 
#include <ESP32Time.h>
#include <WiFiUdp.h> 

// ==========================================
// DEFINIÇÃO DOS PINOS E CONFIGURAÇÕES
// ==========================================
#define PIN_OUT_VERDE    25
#define PIN_OUT_AMARELO  26
#define PIN_OUT_VERMELHO 27
#define PIN_IN_VERDE     18
#define PIN_IN_AMARELO   19
#define PIN_IN_VERMELHO  21
#define ID_SEMAFORO "SEMAFORO-01"
#define PORTA_UDP 8888 

// ==========================================
// VARIÁVEIS GLOBAIS DE ESTADO
// ==========================================
enum EstadoSemaforo { MODO_ROTINA, MODO_CRITICO };
volatile EstadoSemaforo estadoAtual = MODO_ROTINA; 

TickType_t TEMPO_VERDE    = pdMS_TO_TICKS(10000); 
TickType_t TEMPO_AMARELO  = pdMS_TO_TICKS(4000);  
TickType_t TEMPO_VERMELHO = pdMS_TO_TICKS(16000); 

String status_semaforo;

// Handles das Threads
TaskHandle_t TaskPiscaHandle = NULL;
TaskHandle_t TaskStatusHandle = NULL;
TaskHandle_t TaskRedeHandle = NULL; 

SemaphoreHandle_t xSinalFase; 
SemaphoreHandle_t xMutexRotina; 

ESP32Time rtc;
WiFiUDP udp; 

// Assinaturas das Funções
String montarJson(String id_semaforo);
void atualizarFarol(int verde, int amarelo, int vermelho);
void lerControladora(String jsonRecebido);
void ROTINA(int tempo_verde, int tempo_amarelo, int tempo_vermelho);
void CRITICO();
void RESET();

// Threads
void rotina(void * pvParameters);
void status(void * pvParameters);
void escutaRede(void * pvParameters);

// ==========================================
// IMPLEMENTAÇÃO DAS FUNÇÕES DE CONTROLE
// ==========================================

void CRITICO(){
  estadoAtual = MODO_CRITICO; 
  Serial.println("[ESTADO] Transição para MODO_CRITICO efetuada.");
}

void RESET (){
  estadoAtual = MODO_ROTINA; 
  rtc.setTime(1767225600);   
  Serial.println("[ESTADO] RESET efetuado.");
}

void ROTINA(int tempo_verde, int tempo_amarelo, int tempo_vermelho){
  if (xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE) {
    TEMPO_VERDE    = pdMS_TO_TICKS(tempo_verde); 
    TEMPO_AMARELO  = pdMS_TO_TICKS(tempo_amarelo);  
    TEMPO_VERMELHO = pdMS_TO_TICKS(tempo_vermelho); 
    xSemaphoreGive(xMutexRotina);
    Serial.println("[CONFIG] Novos tempos de sincronismo aplicados.");
  }
}

void lerControladora(String jsonRecebido){
  JsonDocument inputDoc;
  DeserializationError erro = deserializeJson(inputDoc, jsonRecebido);

  if (erro) {
    Serial.println("[ERRO] JSON recebido da controladora está corrompido.");
    return;
  }

  if (inputDoc["mensagem"] == "RESET"){
    RESET();
  }
  else if (inputDoc["mensagem"] == "CRITICO"){
    CRITICO();
  }
  else if (inputDoc.containsKey("tempo_verde")) {
    ROTINA(inputDoc["tempo_verde"], inputDoc["tempo_amarelo"], inputDoc["tempo_vermelho"]);
  }
}

// ==========================================
// THREADS DO SISTEMA (FREE RTOS)
// ==========================================

void escutaRede(void * pvParameters) {
  Serial.print("Thread Rede iniciada no núcleo: ");
  Serial.println(xPortGetCoreID());

  udp.begin(PORTA_UDP); 
  char bufferRecebimento[512]; 

  for(;;) {
    int tamanhoPacote = udp.parsePacket();
    if (tamanhoPacote > 0) {
      int len = udp.read(bufferRecebimento, 511);
      bufferRecebimento[len] = '\0'; 
      String jsonString = String(bufferRecebimento);
      lerControladora(jsonString); 
    }
    vTaskDelay(pdMS_TO_TICKS(1)); // Evita starvation em loops rápidos
  }
}

void rotina(void * pvParameters){
  Serial.print("Thread Rotina iniciada no núcleo: ");
  Serial.println(xPortGetCoreID());

  for(;;) {
    if (estadoAtual == MODO_CRITICO) {
      atualizarFarol(0, 1, 0); 
      vTaskDelay(pdMS_TO_TICKS(500));
      atualizarFarol(0, 0, 0); 
      vTaskDelay(pdMS_TO_TICKS(500));
      continue; 
    }

    if (xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE) {
      // FASE: VERMELHO
      atualizarFarol(0, 0, 1);
      xSemaphoreGive(xSinalFase); 
      xSemaphoreGive(xMutexRotina); 
      vTaskDelay(TEMPO_VERMELHO); 

      // FASE: VERDE
      if (estadoAtual == MODO_ROTINA && xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE) {
        atualizarFarol(1, 0, 0);
        xSemaphoreGive(xSinalFase); 
        xSemaphoreGive(xMutexRotina);
        vTaskDelay(TEMPO_VERDE);  
      }

      // FASE: AMARELO
      if (estadoAtual == MODO_ROTINA && xSemaphoreTake(xMutexRotina, portMAX_DELAY) == pdTRUE) {
        atualizarFarol(0, 1, 0);
        xSemaphoreGive(xSinalFase); 
        xSemaphoreGive(xMutexRotina);
        vTaskDelay(TEMPO_AMARELO);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1)); 
  }
}

void status(void * pvParameters){
  Serial.print("Thread Status iniciada no núcleo: ");
  Serial.println(xPortGetCoreID());

  for(;;) {
    if (xSemaphoreTake(xSinalFase, portMAX_DELAY) == pdTRUE) {
      status_semaforo = montarJson(ID_SEMAFORO);
      // send_status(status_semaforo);
    }
  }
}

// ==========================================
// FUNÇÕES AUXILIARES FÍSICAS E JSON
// ==========================================

String montarJson(String id_semaforo){
  JsonDocument doc;

  doc["id_semaforo"] = id_semaforo;
  doc["timestamp"]   = rtc.getTime("%d/%m/%Y %H:%M:%S"); 

  doc["status"]["vermelho"] = digitalRead(PIN_IN_VERMELHO);
  doc["status"]["amarelo"]  = digitalRead(PIN_IN_AMARELO);
  doc["status"]["verde"]    = digitalRead(PIN_IN_VERDE);

  String output;
  serializeJson(doc, output);

  Serial.println("\n--- [EVENTO] Nova Fase Detectada ---");
  Serial.print("JSON Gerado: ");
  Serial.println(output);

  return output;
}

void atualizarFarol(int verde, int amarelo, int vermelho){
  digitalWrite(PIN_OUT_VERDE, verde);
  digitalWrite(PIN_OUT_AMARELO, amarelo);
  digitalWrite(PIN_OUT_VERMELHO, vermelho);
}

// ==========================================
// SETUP E LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  Serial.println("Inicializando Sistema de Semáforo Inteligente...");
  
  pinMode(PIN_OUT_VERDE, OUTPUT);
  pinMode(PIN_OUT_AMARELO, OUTPUT);
  pinMode(PIN_OUT_VERMELHO, OUTPUT);
  pinMode(PIN_IN_VERDE, INPUT);
  pinMode(PIN_IN_AMARELO, INPUT);
  pinMode(PIN_IN_VERMELHO, INPUT);

  xSinalFase = xSemaphoreCreateBinary();
  xMutexRotina = xSemaphoreCreateMutex();

  rtc.setTime(1767225600);

  xTaskCreatePinnedToCore(rotina,      "Rotina", 2048, NULL, 1, &TaskPiscaHandle,  1);
  xTaskCreatePinnedToCore(status,      "Status", 4096, NULL, 1, &TaskStatusHandle, 1);
  xTaskCreatePinnedToCore(escutaRede,  "Rede",   4096, NULL, 2, &TaskRedeHandle,   1); 

  Serial.println("Setup finalizado. Threads recriadas com sucesso!");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}