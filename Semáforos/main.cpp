#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <chrono>
#include "ArduinoJson-v7.4.3.h"

// --- INCLUSÕES PARA REDE NO WINDOWS (WINSOCK) ---
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#define PORT 5000
#define BUFFER_SIZE 4096

// --- BUFFER / FILAS DE COMUNICAÇÃO ENTRE THREADS ---
std::queue<std::string> filaMensagensRecebidas;  // Escuta -> Processa
std::queue<std::string> filaMensagensParaEnviar; // Processa -> Envia

std::mutex mutexFilaRecebidos;
std::condition_variable condFilaRecebidos;

std::mutex mutexFilaEnvio;
std::condition_variable condFilaEnvio;

// --- CONTROLE DA THREAD DE TECLADO ---
std::mutex mutexTeclado;
std::condition_variable condTeclado;
bool aguardandoReset = false;

bool sistemaRodando = true;

// --- SOCKET GLOBAL PARA AS THREADS COMPARTILHAREM A CONEXÃO ---
SOCKET socketClienteAtivo = INVALID_SOCKET;
std::mutex mutexSocket; // Garante que não haverá conflito ao fechar/usar o socket

// Declaração prévia das funções
void ordem(int critico, int reset);
void lerMensagem(std::string jsonRecebido);

// =================================================================
// 1. THREAD: ESCUTA MENSAGENS (RECEBE REAL)
// =================================================================
void threadEscuta() {
    std::cout << "[THREAD ESCUTA] Inicializando Winsock..." << std::endl;

    WSADATA wsaData;
    SOCKET serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    int clientAddrSize = sizeof(clientAddr);
    char buffer[BUFFER_SIZE];

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[THREAD ESCUTA][ERR] Falha ao inicializar Winsock.\n";
        return;
    }

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[THREAD ESCUTA][ERR] Falha no Bind. Porta " << PORT << " em uso.\n";
        WSACleanup();
        return;
    }

    listen(serverSocket, SOMAXCONN);
    std::cout << "[THREAD ESCUTA] Servidor pronto e escutando na porta " << PORT << "...\n";

    while (sistemaRodando) {
        SOCKET tempSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrSize);
        if (tempSocket == INVALID_SOCKET) continue;

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
        std::cout << "\n[THREAD ESCUTA] Cliente conectado de: " << clientIp << "\n";

        // Salva o socket globalmente para a outra thread poder usar para enviar
        {
            std::lock_guard<std::mutex> lock(mutexSocket);
            socketClienteAtivo = tempSocket;
        }

        // Loop de recebimento de dados deste cliente
        while (sistemaRodando) {
            memset(buffer, 0, BUFFER_SIZE);
            int bytesReceived = recv(socketClienteAtivo, buffer, BUFFER_SIZE - 1, 0);

            if (bytesReceived > 0) {
                std::cout << "[THREAD ESCUTA] Recebido via rede: " << buffer;
                std::string jsonVindoDaRede(buffer);
                {
                    std::lock_guard<std::mutex> lock(mutexFilaRecebidos);
                    filaMensagensRecebidas.push(jsonVindoDaRede);
                }
                condFilaRecebidos.notify_one(); 
            } 
            else {
                if (bytesReceived == 0) std::cout << "[THREAD ESCUTA] Cliente desconectou normalmente.\n";
                else std::cout << "[THREAD ESCUTA] Conexao perdida ou erro.\n";
                break; 
            }
        }

        // Limpa o socket global ao desconectar
        {
            std::lock_guard<std::mutex> lock(mutexSocket);
            closesocket(socketClienteAtivo);
            socketClienteAtivo = INVALID_SOCKET;
        }
    }

    closesocket(serverSocket);
    WSACleanup();
}

// =================================================================
// 2. THREAD: PROCESSA MENSAGENS
// =================================================================
void threadProcessa() {
    std::cout << "[THREAD PROCESSA] Inicializada..." << std::endl;

    while (sistemaRodando) {
        std::string jsonParaProcessar;

        {
            std::unique_lock<std::mutex> lock(mutexFilaRecebidos);
            condFilaRecebidos.wait(lock, [] { return !filaMensagensRecebidas.empty() || !sistemaRodando; });
            
            if (!sistemaRodando && filaMensagensRecebidas.empty()) break;

            jsonParaProcessar = filaMensagensRecebidas.front();
            filaMensagensRecebidas.pop();
        }

        lerMensagem(jsonParaProcessar);
    }
}

void lerMensagem(std::string jsonRecebido) {
    JsonDocument inputDoc;
    DeserializationError erro = deserializeJson(inputDoc, jsonRecebido);

    if (erro) {
        std::cout << "[ERRO] JSON recebido do Semaforo esta corrompido.\n";
        return;
    }

    int vermelho = inputDoc["status"]["vermelho"];
    int amarelo  = inputDoc["status"]["amarelo"];
    int verde    = inputDoc["status"]["verde"];

    if ((vermelho + amarelo + verde) == 0) {
        ordem(1, 0); 
    }
    else if ((vermelho + amarelo + verde) > 1) {
        ordem(0, 1); 
    }
}

void ordem(int critico, int reset) {
    JsonDocument docResposta;

    if (critico == 1) {
        docResposta["mensagem"] = "CRITICO";
    }
    if (reset == 1) {
        docResposta["mensagem"] = "RESET";
    }

    std::string output;
    serializeJson(docResposta, output);
    output += "\n"; // Adiciona quebra de linha para o receptor saber que o JSON acabou

    std::cout << "\n--- [ATUALIZACAO] Mensagem de decisao gerada ---" << std::endl;
    std::cout << "JSON Gerado: " << output;

    {
        std::lock_guard<std::mutex> lock(mutexFilaEnvio);
        filaMensagensParaEnviar.push(output);
    }
    condFilaEnvio.notify_one(); 
}

// =================================================================
// 3. THREAD: ENVIA MENSAGENS (AGORA ENVIANDO DE VERDADE VIA SOCKET)
// =================================================================
void threadEnvia() {
    std::cout << "[THREAD ENVIA] Inicializada..." << std::endl;

    while (sistemaRodando) {
        std::string jsonParaEnviar;

        {
            std::unique_lock<std::mutex> lock(mutexFilaEnvio);
            condFilaEnvio.wait(lock, [] { return !filaMensagensParaEnviar.empty() || !sistemaRodando; });

            if (!sistemaRodando && filaMensagensParaEnviar.empty()) break;

            jsonParaEnviar = filaMensagensParaEnviar.front();
            filaMensagensParaEnviar.pop();
        }

        // --- ENVIO REAL AQUI ---
        // Bloqueia o uso do socket para garantir estabilidade
std::lock_guard<std::mutex> lock(mutexSocket);
        
        if (socketClienteAtivo != INVALID_SOCKET) {
            int resultadoEnvio = send(socketClienteAtivo, jsonParaEnviar.c_str(), jsonParaEnviar.length(), 0);
            
            if (resultadoEnvio == SOCKET_ERROR) {
                std::cerr << "[THREAD ENVIA][ERR] Erro ao enviar dados para a ESP32.\n";
            } else {
                std::cout << "[THREAD ENVIA] Enviado com sucesso via rede: " << jsonParaEnviar;
                
                // VERIFICAÇÃO: Se o JSON continha a ordem CRITICO, acorda o terminal
                if (jsonParaEnviar.find("CRITICO") != std::string::npos) {
                    {
                        std::lock_guard<std::mutex> lockTeclado(mutexTeclado);
                        aguardandoReset = true;
                    }
                    condTeclado.notify_one(); // Libera a thread do teclado
                }
            }
        } else {
            std::cout << "[THREAD ENVIA][AVISO] Mensagem ignorada: Nenhuma ESP32 conectada.\n";
        }
    }
}

void threadMonitorTeclado() {
    std::string entradaUsuario;
    const std::string CHAVE_ESPERADA = "RESET";

    while (sistemaRodando) {
        // 1. Fica completamente adormecida até o send() do CRITICO acontecer
        {
            std::unique_lock<std::mutex> lock(mutexTeclado);
            condTeclado.wait(lock, [] { return aguardandoReset || !sistemaRodando; });
        }

        if (!sistemaRodando) break;

        // 2. Acordou. O pacote já foi enviado. Agora trava no getline aguardando o usuário
        std::cout << "\n[TERMINAL] Pacote CRITICO confirmado na rede. Digite 'RESET' e pressione ENTER: ";
        std::getline(std::cin, entradaUsuario);

        // 3. Valida a entrada
        if (entradaUsuario == CHAVE_ESPERADA) {
            std::cout << "[TECLADO] Comando aceito. Gerando ordem de RESET...\n";
            
            // Coloca a nova ordem na FIFO de envio
            ordem(0, 1); 
            
            // Reseta a flag e volta a dormir no próximo ciclo do while
            {
                std::lock_guard<std::mutex> lock(mutexTeclado);
                aguardandoReset = false;
            }
        } else if (!entradaUsuario.empty()) {
            std::cout << "[TECLADO] Comando ignorado: " << entradaUsuario << std::endl;
        }
    }
}

// =================================================================
// FUNÇÃO PRINCIPAL (MAIN)
// =================================================================
int main() {
    std::cout << "=== Inicializando Sistema de Semaforo no PC ===" << std::endl;

    std::thread tEscuta(threadEscuta);
    std::thread tProcessa(threadProcessa);
    std::thread tEnvia(threadEnvia);
    std::thread tTeclado(threadMonitorTeclado);
    std::cout << "Todas as threads rodando em paralelo. Pressione Ctrl+C para encerrar.\n" << std::endl;

    tEscuta.join();
    tProcessa.join();
    tEnvia.join();
    tTeclado.join();

    return 0;
}
