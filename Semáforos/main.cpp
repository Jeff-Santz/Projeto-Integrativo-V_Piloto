#include <iostream>

#include <thread>

#include <queue>

#include <vector>

#include <algorithm>

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



// --- VETOR DE SOCKETS PARA SUPORTAR MÚLTIPLAS ESP32 (BROADCAST) ---

std::vector<SOCKET> clientesAtivos;

std::mutex mutexSocket; // Protege o vetor de clientesAtivos



// Declaração prévia das funções

void ordem(int critico, int reset);

void lerMensagem(std::string jsonRecebido);

void threadClienteHandler(SOCKET clientSocket, std::string clientIp);



// =================================================================

// FUNÇÃO AUXILIAR: REMOVE UM SOCKET DO VETOR DE CLIENTES ATIVOS

// =================================================================

void removerCliente(SOCKET s) {

    std::lock_guard<std::mutex> lock(mutexSocket);

    clientesAtivos.erase(

        std::remove(clientesAtivos.begin(), clientesAtivos.end(), s),

        clientesAtivos.end()

    );

    closesocket(s);

}



// =================================================================

// 1. THREAD: ESCUTA CONEXÕES (ACCEPT) E DELEGA CADA CLIENTE

//    PARA UMA THREAD DEDICADA

// =================================================================

void threadEscuta() {

    std::cout << "[THREAD ESCUTA] Inicializando Winsock..." << std::endl;



    WSADATA wsaData;

    SOCKET serverSocket;

    struct sockaddr_in serverAddr, clientAddr;

    int clientAddrSize = sizeof(clientAddr);



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



    // Loop principal de accept(): cada nova conexão vira uma thread dedicada,

    // permitindo múltiplas ESP32 conectadas simultaneamente.

    while (sistemaRodando) {

        SOCKET novoSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrSize);

        if (novoSocket == INVALID_SOCKET) {

            if (!sistemaRodando) break;

            continue;

        }



        char clientIp[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);

        std::string ipStr(clientIp);

        std::cout << "\n[THREAD ESCUTA] Cliente conectado de: " << ipStr << "\n";



        // Adiciona o novo cliente ao vetor compartilhado

        {

            std::lock_guard<std::mutex> lock(mutexSocket);

            clientesAtivos.push_back(novoSocket);

        }



        // Cada cliente é atendido por sua própria thread de recv(),

        // com seu próprio buffer acumulador (evita mistura de fragmentos

        // de pacotes entre diferentes placas).

        std::thread(threadClienteHandler, novoSocket, ipStr).detach();

    }



    closesocket(serverSocket);

    WSACleanup();

}



// =================================================================

// 1.1 THREAD DEDICADA POR CLIENTE: FAZ O RECV() E MONTA O FIFO

//     DE LEITURA USANDO UMA STRING ACUMULADORA, EXTRAINDO LINHAS

//     COMPLETAS (DELIMITADAS POR '\n') ANTES DE ENFILEIRAR.

// =================================================================

void threadClienteHandler(SOCKET clientSocket, std::string clientIp) {

    char buffer[BUFFER_SIZE];

    std::string acumulador; // Buffer seguro contra fragmentação de pacotes TCP



    while (sistemaRodando) {

        memset(buffer, 0, BUFFER_SIZE);

        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);



        if (bytesReceived > 0) {

            acumulador.append(buffer, bytesReceived);



            // Extrai todas as linhas completas já disponíveis no acumulador.

            // Só faz push() para a fila quando encontra um '\n', garantindo

            // que mensagens fragmentadas pelo TCP sejam remontadas corretamente.

            size_t posQuebra;

            while ((posQuebra = acumulador.find('\n')) != std::string::npos) {

                std::string linhaCompleta = acumulador.substr(0, posQuebra);

                acumulador.erase(0, posQuebra + 1);



                if (!linhaCompleta.empty()) {

                    std::cout << "[THREAD ESCUTA] Recebido de " << clientIp

                              << ": " << linhaCompleta << "\n";

                    {

                        std::lock_guard<std::mutex> lock(mutexFilaRecebidos);

                        filaMensagensRecebidas.push(linhaCompleta);

                    }

                    condFilaRecebidos.notify_one();

                }

            }

        }

        else {

            if (bytesReceived == 0)

                std::cout << "[THREAD ESCUTA] Cliente " << clientIp << " desconectou normalmente.\n";

            else

                std::cout << "[THREAD ESCUTA] Conexao perdida ou erro com " << clientIp << ".\n";

            break;

        }

    }



    removerCliente(clientSocket);

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



    // Lógica mantida exatamente como no original.

    if ((vermelho + amarelo + verde) == 0) {

        ordem(1, 0); // Apagão -> CRÍTICO

    }

    else if ((vermelho + amarelo + verde) > 1) {

        // Conflito lógico -> RESET imediato e incondicional, sem espera

        // ou retenção de estado entre placas: a ordem vai direto para a

        // fila de envio e será propagada via broadcast para todas as ESP32.

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

    output += "\n"; // Delimitador de fim de mensagem para o receptor



    std::cout << "\n--- [ATUALIZACAO] Mensagem de decisao gerada ---" << std::endl;

    std::cout << "JSON Gerado: " << output;



    {

        std::lock_guard<std::mutex> lock(mutexFilaEnvio);

        filaMensagensParaEnviar.push(output);

    }

    condFilaEnvio.notify_one();

}



// =================================================================

// 3. THREAD: ENVIA MENSAGENS (BROADCAST PARA TODAS AS ESP32 ATIVAS)

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



        bool enviouParaAlgumCliente = false;

        std::vector<SOCKET> socketsComFalha;



        {

            std::lock_guard<std::mutex> lock(mutexSocket);



            if (clientesAtivos.empty()) {

                std::cout << "[THREAD ENVIA][AVISO] Mensagem ignorada: Nenhuma ESP32 conectada.\n";

            }



            // Broadcast: envia a mesma ordem para todas as placas conectadas

            for (SOCKET s : clientesAtivos) {

                int resultadoEnvio = send(s, jsonParaEnviar.c_str(), (int)jsonParaEnviar.length(), 0);



                if (resultadoEnvio == SOCKET_ERROR) {

                    std::cerr << "[THREAD ENVIA][ERR] Erro ao enviar dados para socket " << s << ".\n";

                    socketsComFalha.push_back(s);

                } else {

                    std::cout << "[THREAD ENVIA] Enviado com sucesso para socket " << s

                              << ": " << jsonParaEnviar;

                    enviouParaAlgumCliente = true;

                }

            }

        }



        // Remove fora do lock de envio para evitar deadlock com removerCliente()

        for (SOCKET s : socketsComFalha) {

            removerCliente(s);

        }



        // Se a ordem CRITICO foi efetivamente enviada a pelo menos uma placa,

        // acorda a thread do teclado aguardando confirmação de RESET.

        if (enviouParaAlgumCliente && jsonParaEnviar.find("CRITICO") != std::string::npos) {

            {

                std::lock_guard<std::mutex> lockTeclado(mutexTeclado);

                aguardandoReset = true;

            }

            condTeclado.notify_one();

        }

    }

}



void threadMonitorTeclado() {

    std::string entradaUsuario;

    const std::string CHAVE_ESPERADA = "RESET";



    while (sistemaRodando) {

        // 1. Fica completamente adormecida até o broadcast de CRITICO acontecer

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



            // Coloca a nova ordem na FIFO de envio (será propagada via broadcast)

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

