#include <SPI.h>
#include <Ethernet.h>

#define LED_YELLOW 2
#define VSPI_SCK 18
#define VSPI_MISO 19
#define VSPI_MOSI 23
#define W5500_CS 5 
#define SPI_FRQ 32000000

byte mac[] = {  0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192,168,10,2);

// CORREÇÃO 1: Faltava definir o IP do seu computador (Servidor)
IPAddress serverIP(192,168,10,1); // <-- Altere para o IP real da sua máquina na rede
const uint16_t serverPort = 5000;

// CORREÇÃO 2: Instanciar o objeto client globalmente
EthernetClient client; 

void setup() {
  Serial.begin(115200);
  pinMode(LED_YELLOW, OUTPUT);
  digitalWrite(LED_YELLOW, LOW); 

  SPI.setFrequency(SPI_FRQ);
  
  Serial.println("Starting ethernet");
  Ethernet.init(W5500_CS); 
  Ethernet.begin(mac,ip);
  delay(1000);
  Serial.println(Ethernet.localIP());
}

void sendPck(
    uint8_t sourceNode,
    uint8_t destinationNode,
    uint8_t state)
{
    uint32_t timestamp = millis();

    // CORREÇÃO 3: Tudo alterado para 'client' minúsculo (o objeto que instanciamos)
    if (!client.connected())
    {
        Serial.println("Conectando ao servidor...");

        if (!client.connect(serverIP, serverPort))
        {
            Serial.println("Falha na conexão.");
            return;
        }

        Serial.println("Conectado!");
    }

    client.print(timestamp);
    client.print(",");
    client.print(sourceNode);
    client.print(",");
    client.print(destinationNode);
    client.print(",");
    client.println(state);

    Serial.print("Pacote enviado: ");
    Serial.print(timestamp);
    Serial.print(",");
    Serial.print(sourceNode);
    Serial.print(",");
    Serial.print(destinationNode);
    Serial.print(",");
    Serial.println(state);
}

unsigned long lastSend = 0;

void loop() {
      if (millis() - lastSend >= 5000)
    {
        lastSend = millis();
        uint8_t state = random(0,3);

        sendPck(
            2,      // Nó origem
            0,      // Nó destino (Master)
            state
        );
    }
}