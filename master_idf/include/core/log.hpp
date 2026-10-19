#pragma once
// Logger minimo com timestamp e nivel, thread-safe (mutex no stdout).
// Nao e nenhuma biblioteca chique de proposito -- isso aqui e uma
// ferramenta de debug de bancada, nao um produto.
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace log_ {

inline std::mutex& mutexSaida()
{
    static std::mutex m;
    return m;
}

inline std::string horaAgora()
{
    using namespace std::chrono;
    auto agora = system_clock::now();
    auto ms = duration_cast<milliseconds>(agora.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(agora);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

inline void linha(const char* nivel, const char* tag, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(mutexSaida());
    std::cout << '[' << horaAgora() << "] [" << nivel << "] [" << tag << "] " << msg << '\n';
}

// DEBUG: detalhe de protocolo (control transfers, bytes crus, etc).
// Deixa ligado durante a validacao com hardware; da pra silenciar depois
// comentando o corpo ou adicionando uma flag de verbosidade.
inline void debug(const char* tag, const std::string& msg) { linha("DEBUG", tag, msg); }
inline void info (const char* tag, const std::string& msg) { linha("INFO ", tag, msg); }
inline void warn (const char* tag, const std::string& msg) { linha("WARN ", tag, msg); }
inline void erro (const char* tag, const std::string& msg) { linha("ERRO ", tag, msg); }

} // namespace log_
