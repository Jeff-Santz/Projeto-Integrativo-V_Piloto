#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <ctime>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 5000
#define BUFFER_SIZE 4096

SOCKET activeClientSocket = INVALID_SOCKET;
std::mutex socketMutex;

// ---------------------------------------------------------
// THREAD AUTOMÁTICA: Envia pacotes a cada 10 segundos
// ---------------------------------------------------------
void threadEnvioAutomatico() {
    while (true) {
        // Aguarda 10 segundos antes do primeiro disparo
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // 1. Envia Payload 1 (Failsafe / CRITICO)
        std::string payloadCritico = "{\"mensagem\":\"CRITICO\"}\n";
        socketMutex.lock();
        if (activeClientSocket != INVALID_SOCKET) {
            send(activeClientSocket, payloadCritico.c_str(), payloadCritico.length(), 0);
            std::cout << "\n[MASTER -> ESP32] " << payloadCritico;
        }
        socketMutex.unlock();

        // 2. Delay de 10 segundos 
        std::this_thread::sleep_for(std::chrono::milliseconds(10000));

        // 3. Envia Payload 2 (Voltar para Rotina)
        // NOTA: A sua ESP32 espera "RESET" e não "MODO_ROTINA" para chamar a função RESET()
        std::string payloadRotina = "{\"mensagem\":\"RESET\"}\n";
        socketMutex.lock();
        if (activeClientSocket != INVALID_SOCKET) {
            send(activeClientSocket, payloadRotina.c_str(), payloadRotina.length(), 0);
            std::cout << "[MASTER -> ESP32] " << payloadRotina;
        }
        socketMutex.unlock();

        // 4. Delay de 20 segundos
        std::this_thread::sleep_for(std::chrono::milliseconds(20000));

        // 5. Atualiza o tempo e envia Payload 3 (Timestamp)
        long long epochTime = std::time(nullptr);
        std::string payloadTime = "{\"timestamp\":" + std::to_string(epochTime) + "}\n";
        
        socketMutex.lock();
        if (activeClientSocket != INVALID_SOCKET) {
            send(activeClientSocket, payloadTime.c_str(), payloadTime.length(), 0);
            std::cout << "[MASTER -> ESP32] " << payloadTime;
        }
        socketMutex.unlock();
    }
}

// ---------------------------------------------------------
// LOOP PRINCIPAL (MAIN): Aceita conexões e lê dados (DUMP)
// ---------------------------------------------------------
int main() {
    WSADATA wsaData;
    SOCKET serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    int clientAddrSize = sizeof(clientAddr);
    char buffer[BUFFER_SIZE];

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[ERR] Falha ao inicializar Winsock.\n";
        return 1;
    }

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[ERR] Falha no Bind. Porta " << PORT << " em uso.\n";
        return 1;
    }

    listen(serverSocket, SOMAXCONN);
    std::cout << "[SYSTEM] Escutando a porta " << PORT << "...\n";
    std::cout << "[SYSTEM] Iniciando envios periodicos de teste (10s).\n";
    std::cout << "=========================================================\n";

    // Dispara a thread de envio em background
    std::thread txThread(threadEnvioAutomatico);
    txThread.detach(); 

    while (true) {
        SOCKET tempSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrSize);
        if (tempSocket == INVALID_SOCKET) continue;

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
        
        std::cout << "\n[NET] NOVA CONEXAO: " << clientIp << "\n";
        std::cout << "--- INICIO DO STREAM ---\n";

        socketMutex.lock();
        activeClientSocket = tempSocket;
        socketMutex.unlock();

        while (true) {
            memset(buffer, 0, BUFFER_SIZE);
            int bytesReceived = recv(activeClientSocket, buffer, BUFFER_SIZE - 1, 0);

            if (bytesReceived > 0) {
                std::cout << buffer; 
            } 
            else {
                std::cout << "\n--- FIM DO STREAM ---\n";
                if (bytesReceived == 0) std::cout << "[NET] Desconexao limpa pelo cliente.\n";
                else std::cout << "[NET] Conexao derrubada/Erro.\n";
                break; 
            }
        }
        
        socketMutex.lock();
        closesocket(activeClientSocket);
        activeClientSocket = INVALID_SOCKET;
        socketMutex.unlock();
        
        std::cout << "=========================================================\n";
    }

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}