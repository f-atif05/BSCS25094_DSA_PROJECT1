#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <sys/socket.h>
#include <vector>
#include <cstdio>
using namespace std;


inline constexpr int32_t MAX_VARS_PER_FRAME = 16;
inline constexpr int32_t MAX_STACK_DEPTH = 64;
inline constexpr int IO_BUFFER_SIZE = 64 * 1024;

inline constexpr uint8_t STATUS_OK = 0;
inline constexpr uint8_t STATUS_ERROR = 1;
inline constexpr size_t CRC_SIZE = 4;

inline constexpr const char* DEFAULT_HOST = "127.0.0.1";
inline constexpr int DEFAULT_LB_PORT = 5000;
inline constexpr int DEFAULT_SERVER_PORT = 5001;

inline constexpr int CLIENT_QUEUE_TIMEOUT_SEC = 30;

inline constexpr const char* ERR_SERVER_BUSY = "server busy, try again later";
inline constexpr const char* ERR_BACKEND_FAILED = "backend server failed to start";
inline constexpr const char* ERR_UPLOAD_CHECKSUM = "checksum mismatch, upload corrupted";
inline constexpr const char* ERR_REPLY_CHECKSUM = "checksum mismatch, response corrupted";
inline constexpr const char* ERR_DAMAGED_TDBG = "server sent a damaged .tdbg file";

inline constexpr int64_t TTDB_HEADER_DISK_SIZE = 20;
inline constexpr int32_t MAX_FUNCS = 100;

// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

//cllient needs the same structs as server so adding them here too
struct Variable {
    string name;

    int32_t value;
};
struct Frame {
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot{


    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader {
    char magic[4]; 

    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};


struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};
inline bool recvAll(int sock, char* buf, size_t n) {
    size_t x = 0;

    while (x < n) {
        ssize_t a = recv(sock, buf + x, n - x, 0);
        if (a > 0) {
            x = x + (size_t)a;
            continue;
        }
        if (a == 0) { 
            return false; // havent all the bytes

        }
        if  (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;


}
inline bool sendAll(int sock, const char* buf, size_t n) {
    size_t x = 0;

    while (x < n) {
        ssize_t a = send(sock, buf + x, n - x, MSG_NOSIGNAL);
        if (a > 0) {
            x = x + (size_t)a;
            continue;
        }
        if (a == 0) {
            return false; // havent all the bytes

        }
        if (a < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;


}
inline const uint32_t* crcTable() {
    static const struct Table {
        uint32_t x[256];
        Table() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) {
                    if (c & 1) {

                        c = 0xEDB88320u ^ (c >> 1);
                    }
                    else {
                        c = c >> 1;

                    }
                }

                x[i] = c;
            }
        }
    } table;

    return table.x;
}
class Crc32 {
    uint32_t state;
public:
    Crc32() {
        state = 0xFFFFFFFFu;

    }
    void reset() {
        state = 0xFFFFFFFFu;

    }
    void update(const void* data, size_t n) {
        const uint32_t* t = crcTable();
        const unsigned char* p = (const unsigned char*)data;
        uint32_t c = state;
        for (size_t i = 0; i < n; i++) {
            c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);

        }
        state =  c;


    }
    uint32_t value() const {
        return state ^ 0xFFFFFFFFu;

    }
};
inline void putU32LE(uint32_t a, char x[4]) {

    x[0] = (char)(a & 0xFF);

    x[1] = (char)((a >> 8) & 0xFF);
    x[2] = (char)((a  >> 16) & 0xFF);
    x[3] = (char)((a >> 24) & 0xFF);
}
inline uint32_t getU32LE(const char x[4]) {

   uint32_t f = (uint32_t(unsigned char)x[0]) | 
       ((uint32_t)(unsigned char)x[1] << 8) | 
       ((uint32_t)(unsigned char)x[2] << 16)| 
       
       ((uint32_t)(unsigned char)x[3] << 24);
   return f;


}


class TrailerChecker {
private:
    Crc32 crc;
    std::string held;

    uint64_t released_t;

public:
    TrailerChecker() {
        released_t = 0;
    }
    void reset() {
        crc.reset();
        held.clear();
        released_t = 0;


    }
    void feed(const char* data, size_t n,  std::string& released) {
        held.append(data, n);
        if (held.size() > CRC_SIZE) {

            size_t x = held.size() - CRC_SIZE;
            crc.update(held.data(), x);
            released.append(held, 0, x);
            released_t += x;

            held.erase(0, x);

        }
    }
    bool verify() const {
        bool f = held.size() == CRC_SIZE && getU32LE(held.data()) == crc.value();

        return f;

    
    }
    const char* trailer() const{

        return held.data();
    }
    bool hasTrailer() const {
        return held.size() == CRC_SIZE;

    }

    uint64_t releasedBytes() const{

        return released_t;
    }
};

inline bool sendFramedMessage(int sock, uint8_t stat, const std::string& msg) {


    Crc32 crcc;
    char s = char(stat);
    crcc.update(&s, 1);

    crcc.update(msg.data(), msg.size());
    std::string wire;
    wire.push_back(s);
    wire.append(msg);
    char tail[CRC_SIZE];
    putU32LE(crcc.value(), tail);
    wire.append(tail, CRC_SIZE);


    return sendAll(sock,  wire.data(),  wire.size());




}

inline bool sendFramedFile(int sock, uint8_t stat, const string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;

    }

    Crc32 crc;
    char stat_byte = (char)stat;
    crc.update(&stat_byte, 1);

    bool send_successful = sendAll(sock, &stat_byte, 1);
    std::vector<char> buffer(IO_BUFFER_SIZE);
    
    while (send_successful) {
        size_t bytes_read = fread(buffer.data(), 1, buffer.size(), f);
        if (bytes_read == 0) {
            break;


        }
        crc.update(buffer.data(), bytes_read);

        send_successful = sendAll(sock, buffer.data(), bytes_read);


    }
    bool read_err = (ferror(f) != 0);

    fclose(f);
    if (!send_successful || read_err) {
        return false;

    }
    char tail[CRC_SIZE];
    putU32LE(crc.value(), tail);
    return sendAll(sock, tail, CRC_SIZE);



}
    