#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

// Força o link da biblioteca Winsock no MSVC/MinGW
#pragma comment(lib, "ws2_32.lib")

#define PORT 5000
#define BUFFER_SIZE 1024

// Função para quebrar a string CSV da ESP32 e imprimir formatado
void parseAndPrint(const std::string& payload) {
    std::stringstream ss(payload);
    std::string item;
    std::vector<std::string> tokens;

    // A ESP32 envia: timestamp,sourceNode,destinationNode,state\r\n
    while (std::getline(ss, item, ',')) {
        tokens.push_back(item);
    }

    if (tokens.size() >= 4) {
        std::cout << "[RX] Origem: Nó " << tokens[1] 
                  << " -> Destino: Nó " << tokens[2] 
                  << " | Estado: " << tokens[3] 
                  << " | Uptime: " << tokens[0] << " ms\n";
    } else {
        std::cout << "[RX RAW] " << payload << "\n";
    }
}

int main() {
    WSADATA wsaData;
    SOCKET serverSocket, clientSocket;
    struct sockaddr_in serverAddr, clientAddr;
    int clientAddrSize = sizeof(clientAddr);
    char buffer[BUFFER_SIZE];

    // 1. Inicializa o Winsock (Obrigatório no Windows)
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Falha ao inicializar Winsock.\n";
        return 1;
    }

    // 2. Cria o Socket TCP
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Erro ao criar socket.\n";
        WSACleanup();
        return 1;
    }

    // 3. Configura IP e Porta (Ouve em todas as interfaces)
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    // 4. Faz o Bind e começa a ouvir
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Falha no Bind. Porta " << PORT << " pode estar em uso.\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    listen(serverSocket, SOMAXCONN);
    std::cout << "[SYSTEM] Servidor TCP Master rodando na porta " << PORT << "...\n";

    // 5. Loop infinito para aceitar conexões e ler dados
    while (true) {
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrSize);
        if (clientSocket == INVALID_SOCKET) continue;

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
        std::cout << "\n[NETWORK] ESP32 Conectada: " << clientIp << "\n";

        // Loop de leitura da conexão ativa
        while (true) {
            memset(buffer, 0, BUFFER_SIZE);
            int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);

            if (bytesReceived > 0) {
                // Remove o '\r' e '\n' que o client.println() do Arduino insere
                std::string data(buffer);
                data.erase(data.find_last_not_of(" \n\r\t") + 1);
                
                parseAndPrint(data);
            } 
            else if (bytesReceived == 0) {
                std::cout << "[NETWORK] ESP32 Desconectou de forma limpa.\n";
                break; // Sai do loop de leitura e volta a esperar nova conexão
            } 
            else {
                std::cout << "[NETWORK] Erro na conexão ou cabo desconectado.\n";
                break;
            }
        }
        closesocket(clientSocket);
    }

    // Limpeza (Em um servidor real, capturaria Ctrl+C para chegar aqui)
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}